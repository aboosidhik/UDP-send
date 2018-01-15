https://www.ffmpeg.org/doxygen/trunk/dump_8c_source.html#l00454
- fps = `st->avg_frame_rate`
- tbr = `st->r_frame_rate`
- tbn = `st->time_base`
- tbc = `st->codec->time_base`

## `AVStream::avg_frame_rate`
Average framerate.
- demuxing: May be set by libavformat when creating the stream or in `avformat_find_stream_info()`.
- muxing: May be set by the caller before `avformat_write_header()`.

## `AVStream::r_frame_rate`
Real base framerate of the stream. This is the lowest framerate with which all timestamps can be represented accurately (it is the least common multiple of all framerates in the stream). Note, this value is just a guess! For example, if the time base is 1/90000 and all frames have either approximately 3600 timer ticks, then `r_frame_rate` will be 25/1.

## `AVStream::time_base`
This is the fundamental unit of time (in seconds) in terms of which frame timestamps are represented.
- decoding: set by libavformat
- encoding: May be set by the caller before `avformat_write_header()` to provide a hint to the muxer about the desired timebase. In `avformat_write_header()`, the muxer will overwrite this field with the timebase that will actually be used for the timestamps written into the file (which may or may not be related to the user-provided one, depending on the format). 

## `AVCodecContext::time_base`
This is the fundamental unit of time (in seconds) in terms of which frame timestamps are represented.
For fixed-fps content, timebase should be 1/framerate and timestamp increments should be identically 1.
- encoding: MUST be set by user.
- decoding: Set by libavcodec.
