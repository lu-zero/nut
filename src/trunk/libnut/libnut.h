// (C) 2005-2006 Oded Shimon
// This file is available under the MIT/X license, see COPYING

#ifndef _NUT_H
#define _NUT_H

/// \defgroup common  Common Defines and Enums
/// \defgroup muxer   libnut Muxer
/// \defgroup demuxer libnut Demuxer

/// \addtogroup common
/// @{
#define NUT_VERSION 2 ///< Version of NUT specification this library implements

enum nut_stream_class_t {
	NUT_VIDEO_CLASS    = 0, ///< = 0
	NUT_AUDIO_CLASS    = 1, ///< = 1
	NUT_SUBTITLE_CLASS = 2, ///< = 2
	NUT_USERDATA_CLASS = 3, ///< = 3
};

/// Frame flags bitfield (several flags may be set at once)
enum nut_frame_flags_t {
	NUT_FLAG_KEY = 1,  ///< Marks frame as keyframe
	NUT_FLAG_EOR = 2,  ///< Marks end of relavence for stream. #NUT_FLAG_KEY \b must be set together with this flag.
};
/// @}

typedef struct nut_context_s nut_context_t;

/// Memory allocation function pointers \ingroup demuxer muxer
typedef struct {
	void * (*malloc)(size_t size);             ///< Memory allocation malloc function pointer
	void * (*realloc)(void *ptr, size_t size); ///< Memory allocation realloc function pointer
	void (*free)(void *ptr);                   ///< Memory allocation free function pointer
} nut_alloc_t;

/// Timebase struct \ingroup demuxer muxer
typedef struct {
	int nom; ///< Example: 1001
	int den; ///< Example: 24000
} nut_timebase_t;

/// Stream header struct \ingroup demuxer muxer
typedef struct {
	int type;                 ///< Possible values are enum ::nut_stream_class_t. Value of -1 terminates a stream header array
	int fourcc_len;           ///< Length of fourcc
	uint8_t * fourcc;         ///< fourcc in big-endian format
	nut_timebase_t time_base; ///< Timebase of stream
	int fixed_fps;            ///< Flag if stream is fixed fps or not
	int decode_delay;         ///< Decode delay of codec in this stream
	int codec_specific_len;   ///< Length of codec specific data
	uint8_t * codec_specific; ///< Codec specific data. May be NULL if #codec_specific_len is zero.
	uint64_t max_pts;         ///< Only used in demuxer. If non-zero, then it is the highest value in stream

	/// \name Video
	/// Only used is type is #NUT_VIDEO_CLASS @{
	int width;                ///< Width of video in pixels
	int height;               ///< Height of video in pixels
	int sample_width;         ///< Ratio to stretch the video. May only be zero if #sample_height is zero
	int sample_height;        ///< Ratio to stretch the video. May only be zero if #sample_width is zero
	int colorspace_type;

	/// \name audio
	/// Only used is type is #NUT_AUDIO_CLASS @{
	int samplerate_nom;       ///< Sample rate of audio. Example: 44100
	int samplerate_denom;     ///< Sample rate denominator of audio. Example: 1
	int channel_count;        ///< Amount of audio channels
} nut_stream_header_t;

/// Single info field struct \ingroup demuxer muxer
typedef struct {
	char type[7];      ///< NULL-terminated string
	char name[65];     ///< NULL-terminated string. Name of info field
	int64_t val;       ///< Meaning of value defined by #type
	int den;           ///< Used if #type is "r". #val is the numerator
	nut_timebase_t tb; ///< Used if #type is "t"
	uint8_t * data;    ///< Used if #type is non-numeric
} nut_info_field_t;

/// Single info packet struct \ingroup demuxer muxer
typedef struct {
	int count;                 ///< Indicates how many info fields are provided in #fields
	int stream_id_plus1;       ///< Zero indicates non-stream-specific info packet
	int chapter_id;            ///< Indicates which subsection of file this info packet applies to
	nut_timebase_t chapter_tb; ///< Timebase of #chapter_start and #chapter_len
	uint64_t chapter_start;    ///< Start of chapter or complete file
	uint64_t chapter_len;      ///< Length of chapter or complete file
	nut_info_field_t * fields; ///< Info fields, has #count elements
} nut_info_packet_t;

/// Single frame packet struct \ingroup demuxer muxer
typedef struct {
	int len;          ///< Length of frame in bytes. \b Must be zero if #NUT_FLAG_EOR is set.
	int stream;       ///< Stream index of frame
	uint64_t pts;     ///< Presentation timestamp of frame
	int flags;        ///< Frame flags from #nut_frame_flags_t
	int64_t next_pts; ///< Only used in muxer. Only necessary if nut_write_frame_reorder() is used.
} nut_packet_t;



/*****************************************
 * Muxer                                 *
 *****************************************/

/// \addtogroup muxer
/// @{

/// Output stream struct
typedef struct {
	void * priv;                                                ///< Opaque priv pointer to be given to function calls
	int (*write)(void * priv, size_t len, const uint8_t * buf); ///< If NULL, nut_output_stream_t::priv is used as FILE*
} nut_output_stream_t;

/// NUT Framecode table input
typedef struct {
	int flag;   ///< Flags of framecode entry.
	int pts;    ///< pts delta from previous frame
	int stream; ///< stream_id of frame
	int mul;    ///< Multiplier for coded frame size
	int size;   ///< LSB for coded frame size
	int count;  ///< Explicit count of framecode entry, \b should be (mul-size) in almost all cases.
} nut_frame_table_input_t;

/// Muxer options struct
typedef struct {
	nut_output_stream_t output;    ///< Output stream function pointers
	nut_alloc_t alloc;             ///< Memory allocation function pointers
	int write_index;               ///< Writes index at end-of-file
	int realtime_stream;           ///< Implies no write_index
	int max_distance;              ///< Valid values from 32-65536. Recommended value is 32768. Lower values give better seekability and error detection and recovery but cause higher overhead.
	nut_frame_table_input_t * fti; ///< Framecode table. May be NULL.
} nut_muxer_opts_t;

/// Allocates NUT muxer context and writes headers to file
nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[], const nut_info_packet_t info[]);

/// Deallocates NUT muxer context
void nut_muxer_uninit(nut_context_t * nut);

/// Writes a single frame to NUT file
void nut_write_frame(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf);

/// Write a single info packet to NUT file
void nut_write_info(nut_context_t * nut, const nut_info_packet_t * info);

/// Buffers and sorts a single frame to be written NUT file
void nut_write_frame_reorder(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf);

/// Flushes reorder buffer and deallocates NUT muxer context
void nut_muxer_uninit_reorder(nut_context_t * nut);

/// Creates an optimized framecode table for NUT main header based on stream info
void nut_framecode_generate(const nut_stream_header_t s[], nut_frame_table_input_t fti[256]);
/// @}



/*****************************************
 * Demuxer                               *
 *****************************************/

/// \addtogroup demuxer
/// @{

/// Input stream struct
typedef struct {
	void * priv;                                            ///< Opaque priv pointer to be given to function calls
	size_t (*read)(void * priv, size_t len, uint8_t * buf); ///< Input stream read function. Must return amount of bytes actually read.
	off_t (*seek)(void * priv, long long pos, int whence);  ///< Input stream seek function. Must return position in file after seek.
	int (*eof)(void * priv);                                ///< Returns if EOF has been met in stream in case of read error.
	off_t file_pos;                                         ///< File position at begginning of read.
} nut_input_stream_t;

/// Demuxer options struct
typedef struct {
	nut_input_stream_t input;  ///< Input stream function pointers
	nut_alloc_t alloc;         ///< Memory allocation function pointers
	int read_index;            ///< Seeks to end-of-file at begginning of playback to search for index. Implies cache_syncpoints
	int cache_syncpoints;      ///< Improoves seekability and error recovery greatly, but costs some memory (0.5mb for very large files).
	void * info_priv;          ///< Opaque priv pointer to be given to #new_info
	void (*new_info)(void * priv, nut_info_packet_t * info); ///< Function to be called when info is found mid-stream. May be NULL.
} nut_demuxer_opts_t;

enum nut_errors {
	NUT_ERR_NO_ERROR      = 0,    ///< = 0
	NUT_ERR_EOF           = 1,    ///< = 1
	NUT_ERR_EAGAIN        = 2,    ///< = 2
	NUT_ERR_OUT_OF_MEM    = 3,    ///< = 3
	NUT_ERR_NOT_SEEKABLE,         ///< Can only be returned by nut_seek(). Indicates that the seek was not successful.
	NUT_ERR_GENERAL_ERROR,
	NUT_ERR_BAD_VERSION,
	NUT_ERR_NOT_FRAME_NOT_N,
	NUT_ERR_BAD_CHECKSUM,
	NUT_ERR_MAX_SYNCPOINT_DISTANCE,
	NUT_ERR_MAX_DISTANCE,
	NUT_ERR_NO_HEADERS,
	NUT_ERR_OUT_OF_ORDER,
	NUT_ERR_MAX_PTS_DISTANCE,
	NUT_ERR_VLC_TOO_LONG,
	NUT_ERR_BAD_STREAM_ORDER,
	NUT_ERR_NOSTREAM_STARTCODE,
	NUT_ERR_BAD_EOF,
};

/// Creates a NUT demuxer context. Does not read any information from file
nut_context_t * nut_demuxer_init(nut_demuxer_opts_t * dopts);

/// Frees a NUT demuxer context. No other functions can be called after this
void nut_demuxer_uninit(nut_context_t * nut);

/// Read headers and index, \b must be called at begginning
int nut_read_headers(nut_context_t * nut, nut_stream_header_t * s [], nut_info_packet_t * info []);

/// Gets frame header, must be called before each packet
int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd);

/// Just reads the frame \b data, not the header
int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf);

/// Gives human readable description of the error return code of any demuxing function
const char * nut_error(int error);

/// Seeks to requested position in seconds
int nut_seek(nut_context_t * nut, double time_pos, int flags, const int * active_streams);
/// @}



/*****************************************
 * Extended Doxygen Documentation        *
 *****************************************/

/*! \mainpage libnut Documentation
 * \author Oded Shimon <ods15@ods15.dyndns.org>
 * \date 2005-2006
 *
 * Reference implementation for NUT open container format.
 *
 * libnut source code can be downloaded from svn://svn.mplayerhq.hu/nut
 *
 * Copyright of this library is MIT/X license. For more details, see
 * \ref License.
 *
 * For more information on the format, please visit http://www.nut.hu
 */

/*! \page License
 * \verbinclude COPYING
 */

/*! \struct nut_alloc_t
 * libc semantics are assumed to all functions. (realloc must work with NULL or zero size)
 *
 * #malloc function pointer may be NULL. This indicates using libc malloc, realloc and free functions.
 *
 * If #malloc is not NULL, #realloc and #free \b must \b not be NULL.
 */

/*! \struct nut_timebase_t
 * The example shown is if fps is 23.976 (24000/1001). Timebase is the opposite of fps.
 */

/*! \struct nut_info_field_t
 * \par Example:
 * \code
 * char * text = "The Foobar Adventure";
 * nut_info_field_t title = {
 * 	.type = "UTF-8",
 * 	.name = "Title",
 * 	.val = strlen(text),
 * 	.data = text,
 * };
 *
 * nut_info_field_t rational = {
 * 	.type = "r",
 * 	.name = "X-Value close to Pi",
 * 	.val = 355,
 * 	.den = 113,
 * };
 * \endcode
 */

/*! \var char nut_info_field_t::type[7]
 * Type of field value
 * - "v" - Integer value
 * - "s" - Signed integer value
 * - "r" - Fraction rational
 * - "t" - Timestamp
 * - "UTF-8" - UTF-8 coded string
 * - Other - Binary data
 */

/*! \var int64_t nut_info_field_t::val
 * - "v" - Integer value
 * - "s" - Integer value
 * - "r" - Numerator of fraction
 * - "t" - Integer timestamp in timebase #tb
 * - Other - Length of #data in bytes
 *
 * In the case of UTF-8 string, length of #data \b must \b not contain the
 * terminating NUL (U+0000).
 */

/*! \var nut_timebase_t nut_info_field_t::tb
 * In the case of muxer, values of #tb \b must be identical to the
 * timebase of one of the streams.
 */

/*! \var uint8_t * nut_info_field_t::data
 * Even in the case of UTF-8 string, this data is \b not NULL terminated.
 *
 * For muxer, this value \b must be NULL if info field carries no binary
 * data.
 */

/*! \var int nut_info_packet_t::count
 * For arrays of #nut_info_packet_t, the packet with a #count of \a -1
 * terminates the array.
 */

/*! \var nut_info_packet_t::chapter_id
 * Value of 0 indicates info packet applies to complete file.
 *
 * Positive values are real chapters. Real chapters must not overlap. The
 * #chapter_id of a real chapter must not be higher than the total amount
 * of real chapters in the file.
 *
 * Negative values indicate a subsection of file and may overlap.
 *
 * If #chapter_id is 0, #chapter_start and #chapter_len provide length of
 * entire file.
 */

/*! \var nut_info_packet_t::chapter_tb
 * In muxing, values #chapter_tb \b must be identical to the timebase of
 * one of the streams
 */

/*!
 * \var int nut_frame_table_input_t::flag
 *
 * This variable is a bitfield. Valid flags are:
 * -    1  FLAG_KEYFRAME
 * -    2  FLAG_EOR
 * -    8  FLAG_CODED_PTS
 * -   16  FLAG_CODED_STREAM_ID
 * -   32  FLAG_SIZE_MSB
 * -   64  FLAG_CHECKSUM
 * -  128  FLAG_RESERVED
 * - 4096  FLAG_CODED
 * - 8192  FLAG_INVALID
 *
 * Last entry of frame table \b must have flag==-1.
 */

/*! \fn nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[], const nut_info_packet_t info[])
 * \param mopts Muxer options
 * \param s     Stream header data, terminated by \a type=-1.
 * \param info  Info packet data, terminated by \a count=-1. May be \a NULL.
 * \return NUT muxer context
 *
 * In case nut_muxer_opts_t::realtime_stream is set, the first packet given
 * to the nut_output_stream_t::write() function given by muxer options will
 * be all of the main headers. This packet must be given exactly once at
 * the begginning of any stream forwarded out.
 */

/*! \fn void nut_muxer_uninit(nut_context_t *nut)
 * \param nut NUT muxer context
 *
 * Optionally writes index if nut_muxer_opts_t::write_index is set and
 * nut_muxer_opts_t::realtime_stream is unset.
 *
 * \warning Must \b not be used directly if nut_write_frame_reorder() was used.
 * \see nut_muxer_uninit_reorder()
 */

/*! \fn void nut_write_frame(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf)
 * \param nut NUT muxer context
 * \param p   Infomation on the frame written.
 * \param buf Actual data of frame.
 *
 * If nut_muxer_opts_t::realtime_stream realtime_stream is unset, repeated
 * headers will be written at some positions. Syncpoints will be written in
 * accordance to NUT spec. If nut_muxer_opts_t::realtime_stream is set,
 * calling this function will result in a single nut_output_stream_t::write()
 * call, which will be the full frame NUT packet. If the packet starts with
 * a syncpoint startcode, it may be used as a start point after giving the
 * main headers to a new client.
 *
 * \warning
 * The use of this function is discouraged if more than a single stream is
 * used, as frames must meet the NUT specification ordering rule.
 * nut_write_frame_reorder() should be used instead.
 * \sa nut_write_frame_reorder()
 */

/*! \fn void nut_write_info(nut_context_t * nut, const nut_info_packet_t * info)
 * \param nut NUT muxer context
 * \param info A single info packet.
 *
 * The use of this function is \b illegal in non realtime streams, and will
 * do nothing if nut_muxer_opts_t::realtime_stream is not set. The result
 * is a single call to nut_output_stream_t::write() with the NUT info packet.
 */

/*! \fn void nut_write_frame_reorder(nut_context_t * nut, const nut_packet_t * p, const uint8_t * buf)
 * \param nut NUT muxer context
 * \param p   Infomation on the frame written.
 * \param buf Actual data of frame.
 *
 * Uses an internal buffer and sorts the frames to meet NUT's ordering rule.
 * Calls to this function \b must \b not be mixed with calls to
 * nut_write_frame().
 *
 * If this function is used, nut_muxer_uninit_reorder() \b must be used.
 * \sa nut_muxer_uninit_reorder()
 */

/*! \fn void nut_muxer_uninit_reorder(nut_context_t * nut)
 * \param nut NUT muxer context
 *
 * Must be used if nut_write_frame_reorder() was used.
 * \sa nut_muxer_uninit()
 */

/*! \fn void nut_framecode_generate(const nut_stream_header_t s[], nut_frame_table_input_t fti[256])
 * \param s   Stream header data, terminated by \a type=-1.
 * \param fti Output framecode table data. Must be pre-allocated to 256 entries.
 *
 * Creates an optimized framecode table for NUT main header based on stream
 * types and codecs. Currently recognized fourcc values are "mp4v", "h264",
 * "mp3 ", and "vrbs".
 *
 * This function is used directly by nut_muxer_init() if
 * nut_muxer_opts_t::fti is \a NULL.
 */

/*! \addtogroup demuxer
 * All of the demuxer related functions return an integer value
 * representing one of the following return codes:
 * - 0 (=#NUT_ERR_NO_ERROR) Success
 * - #NUT_ERR_EOF           Unexpected or expected EOF
 * - #NUT_ERR_EAGAIN        Insufficient data for demuxing
 * - #NUT_ERR_NOT_SEEKABLE  Unsuccessful seek in nut_seek()
 *
 * \par NUT_ERR_EOF
 * If any function returns EOF, it can be recovered by using nut_seek().
 * The exception to this is nut_read_headers(), then it is a fatal
 * error and only nut_demuxer_uninit() can be called.
 *
 * \par NUT_ERR_EAGAIN
 * Indicates not enough data was given from nut_input_stream_t::read()
 * function, but EOF was not met. Whichever function returned this should
 * be re-called given the exact same params when more data is available.
 * The exception to this is nut_read_frame(), which should be called with
 * an updated \a buf param.
 *
 * \par NUT_ERR_NOT_SEEKABLE
 * Can only be returned from nut_seek(). Indicates that no seek has been
 * performed and that the stream is in the exact same position before the
 * seek.
 *
 * \par Other errors
 * All errors except the list above are fatal.
 * The only legal operation after a fatal error is nut_demuxer_uninit().
 */

/*! \var size_t (*nut_input_stream_t::read)(void * priv, size_t len, uint8_t * buf)
 * If NULL, nut_input_stream_t::priv is used as FILE*, and
 * nut_input_stream_t::seek and nut_input_stream_t::eof are ignored.
 */

/*! \var off_t (*nut_input_stream_t::seek)(void * priv, long long pos, int whence)
 * If NULL (and nut_input_stream_t::read is non-NULL), indicates stream is
 * unseekable. This implies nut_demuxer_opts_t::read_index and
 * nut_demuxer_opts_t::cache_syncpoints to be unset. Any call to
 * nut_seek() with an unseekable stream will yield #NUT_ERR_NOT_SEEKABLE
 *
 * Parameter whence must support the following libc defines:
 * - SEEK_SET - pos is used as offset from begginning of file
 * - SEEK_CUR - pos is used as offset from current position
 * - SEEK_END - pos is used as offset from end of file
 */

/*! \var int (*nut_input_stream_t::eof)(void * priv)
 * Only necessary if stream supports non-blocking mode.
 * Returns non-zero if stream is at EOF, 0 otherwise.
 *
 * This function is called if nut_input_stream_t::read returns less data
 * read than requested. If it returns zero, then the stream is assumed to
 * be lacking data and #NUT_ERR_EAGAIN is raised. Otherwise, #NUT_ERR_EOF
 * is raised.
 *
 * If NULL, then any read error is assumed to be caused by EOF.
 */

/*! \var off_t nut_input_stream_t::file_pos
 * Must contain position in file at begginning of demuxing. Should
 * usually be zero.
 */

/*! \fn int nut_read_headers(nut_context_t * nut, nut_stream_header_t * s [], nut_info_packet_t * info [])
 * \param nut  NUT demuxer context
 * \param s    Pointer to stream header variable to be set to an array
 * \param info Pointer to info header variable, may be NULL.
 *
 * Both `s' and `info' are handeled by context, and are illegal pointers
 * after nut_demuxer_uninit().
 *
 * If main NUT headers are not found at begginning of file and the input
 * stream is seekable, Several parts of the file are searched for the main
 * headers before giving up in accordance to NUT spec (EOF and 2^n
 * locations).
 *
 * Any error returned from this function except #NUT_ERR_EAGAIN is fatal.
 * No other functions may be called until a successful call to
 * nut_read_headers().
 */

/*! \fn int nut_read_next_packet(nut_context_t * nut, nut_packet_t * pd)
 * \param nut NUT demuxer context
 * \param pd  Pointer to frame header struct to be filled with next frame
 *            info.
 *
 * After a successful call to nut_read_next_packet(), nut_read_frame()
 * \b must be called.
 *
 * If nut_demuxer_opts_t::new_info is non-NULL, a new info packet may be
 * seen before decoding of frame header and this function pointer will be
 * called (possibly even several times).
 *
 * If a stream error is detected during decoding of frame header,
 * nut_read_next_packet() will attempt to recover from the error and gives
 * the next undamaged frame in the stream.
 */

/*! \fn int nut_read_frame(nut_context_t * nut, int * len, uint8_t * buf)
 * \param nut NUT demuxer context
 * \param len length of data left to be read
 * \param buf buffer to write the frame data
 *
 * This function must be called \b after nut_read_next_packet().
 *
 * If the function return #NUT_ERR_EAGAIN or #NUT_ERR_EOF, \a len will
 * return the amount of bytes actually read.
 *
 * Data is always written to the \b begginning of the buffer \a buf, so it
 * must be moved accordingly in case of beign called again for
 * #NUT_ERR_EAGAIN.
 *
 * \par Example:
 * \code
 * nut_packet_t pd;
 * int len = pd.len;
 * while((err = nut_read_frame(nut, &len, buf+pd.len-len)) == NUT_ERR_EAGAIN)
 * 	sleep(1);
 * \endcode
 */

/*! \fn int nut_seek(nut_context_t * nut, double time_pos, int flags, const int * active_streams)
 * \param nut            NUT demuxer context
 * \param time_pos       Position to seek to in seconds
 * \param flags          Bitfield given as seek options.
 * \param active_streams -1 terminated list of all streams that are active.
 *                       May be NULL - indicates all streams are active.
 *
 * Flag bitfield options:
 * - 1 RELATIVE: If set, \a time_pos is relative to current position,
 *               otherwise it is absoloute.
 * - 2 FORWARD:  If set, the seek should find the nearest keyframe
 *               after the target position. If unset, the nearest
 *               keyframe before the target position is picked.
 *
 * Active streams decide which keyframe the function will seek to. In
 * backwards seeking, a keyframe for all active streams is garunteed to be
 * found before the target presentation timestamp.
 *
 * In forward seeking, no percise seeking is preformed. The function seeks
 * to the first keyframe for an active stream after the target timestamp.
 *
 * If the function returns #NUT_ERR_NOT_SEEKABLE, no seek was performed.
 * If #NUT_ERR_EOF is returned, requested position is outside bounds of
 * file.
 *
 * After nut_seek, nut_read_next_packet should be called to get the next frame.
 */

#endif // _NUT_H
