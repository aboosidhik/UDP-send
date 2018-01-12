void Encoder::configureVideoStreams()
{
    // Preset options:
    // ultrafast	superfast	veryfast	faster	fast	medium	slow	slower	veryslow	placebo
    // see : http://dev.beandog.org/x264_preset_reference.html
    AVDictionary *optionsDict = NULL;
    
    // careful with ultrafast - it seems to force constrained baseline?; this call also allocates
    av_dict_set( &optionsDict, "preset","superfast", 0 );
    
    // To do -> Introspect and make wise choices here.
    videoStream->codec->width = mDecoder->videoStream->codec->width; // 1920
    videoStream->codec->height = mDecoder->videoStream->codec->height; // 1080?
    videoStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    videoStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codec->profile = FF_PROFILE_H264_HIGH;
    videoStream->codec->level = 41;
    
    // Mezzanine Bitrate
    videoStream->codec->bit_rate = (11 * 1024 * 1024);
    
    // Set our output stream to match our input stream as appropriate
    
    // Timing
    videoStream->avg_frame_rate = mDecoder->videoStream->avg_frame_rate;
    videoStream->codec->framerate = mDecoder->videoStream->codec->framerate;
    av_stream_set_r_frame_rate( videoStream, mDecoder->videoStream->r_frame_rate );
    videoStream->start_time = mDecoder->videoStream->start_time;
    videoStream->duration = mDecoder->videoStream->duration;

    // Attempt to alter videoStream timebase

    videoStream->time_base = mDecoder->videoStream->time_base;
    // ??
//    videoStream->time_base.num = mDecoder->videoStream->time_base.num;
//    videoStream->time_base.den = mDecoder->videoStream->time_base.den / mDecoder->videoStream->codec->ticks_per_frame;
    
    videoStream->codec->time_base = mDecoder->videoStream->codec->time_base;
    //??
//    videoStream->codec->time_base.num = mDecoder->videoStream->codec->time_base.num;
//    videoStream->codec->time_base.den = mDecoder->videoStream->codec->time_base.den / mDecoder->videoStream->codec->ticks_per_frame;
//
//    videoStream->codec->ticks_per_frame = mDecoder->videoStream->codec->ticks_per_frame;
    
    // Source / Display Aspect Ratios
    videoStream->display_aspect_ratio = mDecoder->videoStream->display_aspect_ratio;
    videoStream->codec->sample_aspect_ratio = mDecoder->videoStream->codec->sample_aspect_ratio;
    
    // Color Spaces
    videoStream->codec->colorspace = mDecoder->videoStream->codec->colorspace;
    videoStream->codec->color_primaries = mDecoder->videoStream->codec->color_primaries;
    videoStream->codec->color_trc = mDecoder->videoStream->codec->color_trc;
    videoStream->codec->color_range = mDecoder->videoStream->codec->color_range;
    videoStream->codec->chroma_sample_location = mDecoder->videoStream->codec->chroma_sample_location;
    
    av_dict_free( &optionsDict );
}
