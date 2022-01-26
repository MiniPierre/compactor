/*
 * Copyright 2016-2018, 2022 Internet Corporation for Assigned Names and Numbers.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Developed by Sinodun IT (www.sinodun.com)
 */

#ifndef STREAMWRITER_HPP
#define STREAMWRITER_HPP

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include <lzma.h>

/**
 * \class StreamWriter
 * \brief A basic output file write. Just write to the named file.
 */
class StreamWriter
{
public:
    /**
     * \brief Output file name meaning 'output to stdout'.
     */
    static const std::string& STDOUT_FILE_NAME;

    /**
     * \brief Constructor.
     *
     * Creates and opens the output file.
     *
     * \param name  filename.
     * \param level compression level
     */
    StreamWriter(const std::string& name, unsigned level, bool logging = false);

    /**
     * \brief Destructor.
     *
     * Make sure the stream is closed.
     */
    virtual ~StreamWriter();

    /**
     * \brief Write to the output file.
     *
     * \param str       string to write.
     */
    virtual void writeBytes(const std::string& str);

    /**
     * \brief Write to the output file.
     *
     * \param p       pointer to buffer to write.
     * \param n_bytes bytes to write.
     */
    virtual void writeBytes(const uint8_t *p, std::ptrdiff_t n_bytes);

    /**
     * \brief Return additional extension suggested for output file type.
     */
    static const char* suggested_extension()
    {
        return "";
    }

protected:
    /**
     * \brief The output stream.
     */
    std::ostream* os_;

private:

    /**
     * \brief File output stream, if required.
     */
    std::ofstream ofs_;

    /**
     * \brief The final output filename.
     */
    std::string name_;

    /**
     * \brief The temporary file to write to, renamed to final on close.
     */
    std::string temp_name_;

   /**
    * \brief The temporary file to write to, renamed to final on close.
    */
   bool logging_;

};

/**
 * \class GzipStreamWriter
 * \brief A stream writer that gzips the output.
 *
 * The output filename has the extension `.gz` appended.
 */
class GzipStreamWriter : public StreamWriter
{
public:
    /**
     * \brief Constructor.
     *
     * \param name  filename.
     * \param level compression level
     */
    GzipStreamWriter(const std::string& name, unsigned level, bool logging = false);

    /**
     * \brief Destructor.
     *
     * Make sure the stream is closed.
     */
    virtual ~GzipStreamWriter();

    /**
     * \brief Write to the output file.
     *
     * \param p       pointer to buffer to write.
     * \param n_bytes bytes to write.
     */
    virtual void writeBytes(const uint8_t *p, std::ptrdiff_t n_bytes);

    /**
     * \brief Return additional extension suggested for output file type.
     */
    static const char* suggested_extension()
    {
        return ".gz";
    }

private:
    /**
     * \brief The compression parameters.
     */
    boost::iostreams::gzip_params gzparams_;

    /**
     * \brief The compression output stream.
     */
    boost::iostreams::filtering_ostream gzout_;
};

/**
 * \class XzException
 * \brief Exception thrown for liblzma errors.
 */
class XzException : public std::runtime_error
{
public:
    /**
     * \brief Constructor.
     *
     * \param err the liblzma error.
     */
    explicit XzException(lzma_ret err);

protected:
    /**
     * \brief Create a text error message.
     *
     * \param err the liblzma error.
     * \returns text error message.
     */
    static const char* msg(lzma_ret err);
};

/**
 * \class XzStreamWriter
 * \brief A stream writer that xzs the output.
 *
 * The output filename has the extension `.xz` appended.
 */
class XzStreamWriter : public StreamWriter
{
public:
    /**
     * \brief Constructor.
     *
     * \param name  filename.
     * \param level compression level
     */
    XzStreamWriter(const std::string& name, unsigned level, bool logging = false);

    /**
     * \brief Destructor.
     *
     * Make sure the stream is closed.
     */
    virtual ~XzStreamWriter();

    /**
     * \brief Write to the output file.
     *
     * \param p       pointer to buffer to write.
     * \param n_bytes bytes to write.
     */
    virtual void writeBytes(const uint8_t *p, std::ptrdiff_t n_bytes);

    /**
     * \brief Return additional extension suggested for output file type.
     */
    static const char* suggested_extension()
    {
        return ".xz";
    }

private:
    /**
     * \brief Code the LZMA stream. Write any resulting output.
     *
     * \param action the LZMA action.
     * \returns the result of the LZMA code.
     * \throws XzException for any result not LZMA_OK or LZMA_STREAM_END.
     */
    lzma_ret codeLzmaStream(lzma_action action);

    /**
     * \brief liblzma stream structure.
     */
    lzma_stream xz_stream_;
};

#endif
