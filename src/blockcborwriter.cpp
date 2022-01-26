/*
 * Copyright 2016-2021 Internet Corporation for Assigned Names and Numbers.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Developed by Sinodun IT (www.sinodun.com)
 */

#include <chrono>
#include <stdexcept>

#include <limits.h>
#include <unistd.h>

#include "config.h"

#include "capturedns.hpp"
#include "cborencoder.hpp"
#include "dnsmessage.hpp"
#include "makeunique.hpp"
#include "blockcbor.hpp"
#include "blockcbordata.hpp"
#include "blockcborwriter.hpp"
#include "log.hpp"

namespace {
    byte_string addr_to_string(const IPAddress& addr, const Configuration& config, bool is_client = true)
    {
        unsigned prefix_len =
            addr.is_ipv6()
            ? ( is_client ) ? config.client_address_prefix_ipv6 : config.server_address_prefix_ipv6
            : ( is_client ) ? config.client_address_prefix_ipv4 : config.server_address_prefix_ipv4;
        unsigned prefix_nbytes = (prefix_len + 7) / 8;
        byte_string res = addr.asNetworkBinary();
        res.resize(prefix_nbytes);
        if ( prefix_nbytes > 0 )
            res[prefix_nbytes - 1] &= 0xff << (prefix_nbytes * 8 - prefix_len);
        return res;
    }
}

BlockCborWriter::BlockCborWriter(const Configuration& config,
                                 std::unique_ptr<CborBaseStreamFileEncoder> enc,
                                 bool live)
    : BaseOutputWriter(config),
      output_pattern_(config.output_pattern + enc->suggested_extension(),
                      std::chrono::seconds(config.rotation_period)),
      enc_(std::move(enc)),
      live_(live),
      query_response_(), ext_rr_(nullptr), ext_group_(nullptr),
      last_end_block_statistics_(), need_start_block_stats_(true)
{
    block_cbor::BlockParameters bp;
    config.populate_block_parameters(bp);
    block_parameters_.push_back(bp);

    data_ = make_unique<block_cbor::BlockData>(block_parameters_);
    if ( live_ )
        data_->start_time = std::chrono::system_clock::now();
}

BlockCborWriter::~BlockCborWriter()
{
    close();
}

void BlockCborWriter::close()
{
    if ( enc_->is_open() )
    {
        if ( live_ && !data_->end_time )
            data_->end_time = std::chrono::system_clock::now();
        writeBlock();
        writeFileFooter();
        enc_->close();
    }
}

void BlockCborWriter::writeAE(const std::shared_ptr<AddressEvent>& ae,
                              const PacketStatistics& stats)
{
    if ( !config_.exclude_hints.address_events )
        data_->count_address_event(ae->type(),
                                   ae->code(),
                                   addr_to_string(ae->address(), config_),
                                   ae->address().is_ipv6());
    updateBlockStats(stats);
}

void BlockCborWriter::checkForRotation(const std::chrono::system_clock::time_point& timestamp)
{
    if ( !enc_->is_open() ||
         ( config_.max_output_size.size > 0 &&
           enc_->bytes_written() >= config_.max_output_size.size ) ||
         output_pattern_.need_rotate(timestamp, config_) )
    {
        if ( enc_->is_open() )
        {
            data_->end_time = timestamp;
            close();
            data_->start_time = timestamp;
        }
        filename_ = output_pattern_.filename(timestamp, config_);
        LOG_INFO << "Rotating C_DNS file to " << filename_;
        enc_->open(filename_, config_.log_file_handling);
        writeFileHeader();
    }
}

void BlockCborWriter::startRecord(const std::shared_ptr<QueryResponse>& qr)
{
    if ( data_->is_full() )
    {
        const DNSMessage &d(qr->has_query() ? qr->query() : qr->response());
        data_->end_time = d.timestamp;
        writeBlock();
        data_->start_time = d.timestamp;
    }
    query_response_.clear();
    clear_in_progress_extra_info();
}

void BlockCborWriter::endRecord(const std::shared_ptr<QueryResponse>&)
{
    data_->query_response_items.push_back(std::move(query_response_));
    query_response_.clear();
}

void BlockCborWriter::writeBasic(const std::shared_ptr<QueryResponse>& qr,
                                   const PacketStatistics& stats)
{
    const DNSMessage &d(qr->has_query() ? qr->query() : qr->response());
    block_cbor::QueryResponseItem& qri = query_response_;
    block_cbor::QueryResponseSignature qs;
    const HintsExcluded& exclude = config_.exclude_hints;

    qri.qr_flags = 0;

    updateBlockStats(stats);

    if ( data_->query_response_items.size() == 0 ||
         d.timestamp < data_->earliest_time )
        data_->earliest_time = d.timestamp;

    if ( config_.start_end_times_from_data )
    {
        if ( !data_->end_time || d.timestamp > *(data_->end_time) )
            data_->end_time = d.timestamp;
        if ( !data_->start_time || d.timestamp < *(data_->start_time) )
            data_->start_time = d.timestamp;
    }

    // Basic query signature info.
    if ( !exclude.server_address && d.serverIP)
        qs.server_address = data_->add_address(addr_to_string(*d.serverIP, config_, false));
    if ( !exclude.server_port && d.serverPort)
        qs.server_port = *d.serverPort;
    if ( !exclude.transport )
        qs.qr_transport_flags = block_cbor::transport_flags(*qr);
    if ( !exclude.transaction_type )
        qs.qr_type = block_cbor::transaction_type(*qr);
    if ( !exclude.dns_flags )
        qs.dns_flags = block_cbor::dns_flags(*qr);

    // Basic query/response info.
    if ( !exclude.timestamp )
        qri.tstamp = d.timestamp;
    if ( !exclude.client_address && d.clientIP )
        qri.client_address = data_->add_address(addr_to_string(*d.clientIP, config_));
    if ( !exclude.client_port && d.clientPort )
        qri.client_port = *d.clientPort;
    if ( !exclude.transaction_id )
        qri.id = d.dns.id();
    if ( !exclude.query_qdcount )
        qs.qdcount = d.dns.questions_count();

    // Get first query info.
    if ( d.dns.queries().empty() )
        qri.qr_flags |= block_cbor::QUERY_HAS_NO_QUESTION;
    else
    {
        const auto& query = d.dns.queries().front();
        block_cbor::ClassType ct;

        ct.qtype = query.query_type();
        ct.qclass = query.query_class();
        if ( !exclude.query_class_type )
            qs.query_classtype = data_->add_classtype(ct);
        if ( !exclude.query_name )
            qri.qname = data_->add_name_rdata(query.dname());
    }

    if ( qr->has_query() )
    {
        const DNSMessage &q(qr->query());

        qri.qr_flags |= block_cbor::HAS_QUERY;
        if ( !exclude.query_size && q.wire_size )
            qri.query_size = *q.wire_size;
        if ( !exclude.client_hoplimit && q.hoplimit )
            qri.hoplimit = *q.hoplimit;

        if ( !exclude.query_opcode )
            qs.query_opcode = q.dns.opcode();
        if ( !exclude.query_rcode )
            qs.query_rcode = CaptureDNS::Rcode(q.dns.rcode());
        if ( !exclude.query_ancount )
            qs.query_ancount = q.dns.answers_count();
        if ( !exclude.query_arcount )
            qs.query_nscount = q.dns.authority_count();
        if ( !exclude.query_nscount )
            qs.query_arcount = q.dns.additional_count();

        auto edns0 = q.dns.edns0();
        if ( edns0 )
        {
            if ( !exclude.query_rcode )
                qs.query_rcode = CaptureDNS::Rcode(*qs.query_rcode + (edns0->extended_rcode() << 4));
            qri.qr_flags |= block_cbor::QUERY_HAS_OPT;
            if ( !exclude.query_udp_size )
                qs.query_edns_payload_size = edns0->udp_payload_size();
            if ( !exclude.query_edns_version )
                qs.query_edns_version = edns0->edns_version();
            if ( !exclude.query_opt_rdata )
                qs.query_opt_rdata = data_->add_name_rdata(edns0->rr().data());
        }
    }

    if ( qr->has_response() )
    {
        const DNSMessage &r(qr->response());

        qri.qr_flags |= block_cbor::HAS_RESPONSE;
        if ( !exclude.response_size && r.wire_size )
            qri.response_size = *r.wire_size;
        // Set from response if not already set.
        if ( !exclude.query_opcode && !qs.query_opcode )
            qs.query_opcode = r.dns.opcode();
        if ( !exclude.response_rcode )
            qs.response_rcode = CaptureDNS::Rcode(r.dns.rcode());

        auto edns0 = r.dns.edns0();
        if ( edns0 )
        {
            if ( !exclude.response_rcode )
                qs.response_rcode = CaptureDNS::Rcode(*qs.response_rcode + (edns0->extended_rcode() << 4));
            qri.qr_flags |= block_cbor::RESPONSE_HAS_OPT;
        }

        if ( r.dns.questions_count() == 0 )
            qri.qr_flags |= block_cbor::RESPONSE_HAS_NO_QUESTION;
    }

    if ( qr->has_query() && qr->has_response() && !exclude.response_delay )
        qri.response_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(qr->response().timestamp - qr->query().timestamp);

    if ( !exclude.qr_flags )
        qs.qr_flags = qri.qr_flags;
    if ( !exclude.qr_signature )
        qri.signature = data_->add_query_response_signature(qs);
}

void BlockCborWriter::startExtendedQueryGroup()
{
    if ( !query_response_.query_extra_info )
        query_response_.query_extra_info = make_unique<block_cbor::QueryResponseExtraInfo>();
    ext_group_ = query_response_.query_extra_info.get();
}

void BlockCborWriter::startExtendedResponseGroup()
{
    if ( !query_response_.response_extra_info )
        query_response_.response_extra_info = make_unique<block_cbor::QueryResponseExtraInfo>();
    ext_group_ = query_response_.response_extra_info.get();
}

void BlockCborWriter::endExtendedGroup()
{
    if ( extra_questions_.size() > 0 )
        ext_group_->questions_list = data_->add_questions_list(extra_questions_);
    if ( extra_answers_.size() > 0 )
        ext_group_->answers_list = data_->add_rrs_list(extra_answers_);
    if ( extra_authority_.size() > 0 )
        ext_group_->authority_list = data_->add_rrs_list(extra_authority_);
    if ( extra_additional_.size() > 0 )
        ext_group_->additional_list = data_->add_rrs_list(extra_additional_);

    clear_in_progress_extra_info();
}

void BlockCborWriter::startQuestionsSection()
{
}

void BlockCborWriter::writeQuestionRecord(const CaptureDNS::query& question)
{
    block_cbor::ClassType ct;
    block_cbor::Question q;

    if ( !config_.exclude_hints.query_name )
        q.qname = data_->add_name_rdata(question.dname());
    ct.qtype = question.query_type();
    ct.qclass = question.query_class();
    if ( !config_.exclude_hints.query_class_type )
        q.classtype = data_->add_classtype(ct);
    extra_questions_.push_back(data_->add_question(q));
}

void BlockCborWriter::endSection()
{
}

void BlockCborWriter::startAnswersSection()
{
    ext_rr_ = &extra_answers_;
}

void BlockCborWriter::writeResourceRecord(const CaptureDNS::resource& resource)
{
    block_cbor::ClassType ct;
    block_cbor::ResourceRecord rr;

    if ( !config_.exclude_hints.query_name )
        rr.name = data_->add_name_rdata(resource.dname());
    ct.qtype = resource.query_type();
    ct.qclass = resource.query_class();
    if ( !config_.exclude_hints.query_class_type )
        rr.classtype = data_->add_classtype(ct);
    if ( !config_.exclude_hints.rr_ttl )
        rr.ttl = resource.ttl();
    if ( !config_.exclude_hints.rr_rdata )
        rr.rdata = data_->add_name_rdata(resource.data());
    ext_rr_->push_back(data_->add_resource_record(rr));
}

void BlockCborWriter::startAuthoritySection()
{
    ext_rr_ = &extra_authority_;
}

void BlockCborWriter::startAdditionalSection()
{
    ext_rr_ = &extra_additional_;
}

void BlockCborWriter::writeFileHeader()
{
    constexpr int major_format_index = block_cbor::find_file_preamble_index(block_cbor::FilePreambleField::major_format_version);
    constexpr int minor_format_index = block_cbor::find_file_preamble_index(block_cbor::FilePreambleField::minor_format_version);
    constexpr int private_format_index = block_cbor::find_file_preamble_index(block_cbor::FilePreambleField::private_version);
    constexpr int block_parameters_index = block_cbor::find_file_preamble_index(block_cbor::FilePreambleField::block_parameters);

    enc_->writeArrayHeader(3);
    enc_->write(block_cbor::FILE_FORMAT_ID);

    // File preamble.
    enc_->writeMapHeader(4);
    enc_->write(major_format_index);
    enc_->write(block_cbor::FILE_FORMAT_10_MAJOR_VERSION);
    enc_->write(minor_format_index);
    enc_->write(block_cbor::FILE_FORMAT_10_MINOR_VERSION);
    enc_->write(private_format_index);
    enc_->write(block_cbor::FILE_FORMAT_10_PRIVATE_VERSION);

    enc_->write(block_parameters_index);
    writeBlockParameters();

    // Write file header: Start of file blocks.
    enc_->writeArrayHeader();
}

void BlockCborWriter::writeBlockParameters()
{
    block_cbor::BlockParameters block_parameters;

    config_.populate_block_parameters(block_parameters);

    // Currently we only write one block parameter item.
    enc_->writeArrayHeader(1);
    block_parameters.writeCbor(*enc_);
}

void BlockCborWriter::writeFileFooter()
{
    enc_->writeBreak();
}

void BlockCborWriter::writeBlock()
{
    data_->last_packet_statistics = last_end_block_statistics_;
    data_->writeCbor(*enc_);
    data_->clear();
    need_start_block_stats_ = true;
}

void BlockCborWriter::updateBlockStats(const PacketStatistics& stats)
{
    if ( need_start_block_stats_ )
    {
        data_->start_packet_statistics = last_end_block_statistics_;
        need_start_block_stats_ = false;
    }
    last_end_block_statistics_ = stats;
}
