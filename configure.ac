AC_INIT([Janus Gateway],[0.2.2],[https://github.com/meetecho/janus-gateway],[janus-gateway],[https://janus.conf.meetecho.com])
AC_CONFIG_AUX_DIR([.])
AC_CONFIG_MACRO_DIR([m4])

AC_ENABLE_SHARED(yes)
AC_ENABLE_STATIC(no)

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])

AC_GNU_SOURCE

AC_PROG_CC

LT_PREREQ([2.2])
LT_INIT

case "$host_os" in
darwin*)
	CFLAGS="$CFLAGS -I/usr/local/opt/openssl/include -I/usr/local/include"
	LDFLAGS="$LDFLAGS -L/usr/local/lib -L/usr/local/opt/openssl/lib -L/opt/local/lib -L/usr/local/libsrtp/lib"
	AM_CONDITIONAL([DARWIN_OS], true)
	AM_CONDITIONAL([HAS_DTLS_WINDOW_SIZE], false)
;;
*)
	AM_CONDITIONAL([DARWIN_OS], false)
	AM_CONDITIONAL([HAS_DTLS_WINDOW_SIZE], true)
esac

glib_version=2.32
ssl_version=1.0.1

##
# Janus
##

AC_ARG_ENABLE([docs],
              [AS_HELP_STRING([--enable-docs],
                              [Enable building documentation])],
              [],
              [enable_docs=no])

AC_ARG_ENABLE([rest],
              [AS_HELP_STRING([--disable-rest],
                              [Disable REST (HTTP/HTTPS) support])],
              [],
              [enable_rest=maybe])

AC_ARG_ENABLE([websockets],
              [AS_HELP_STRING([--disable-websockets],
                              [Disable WebSockets support])],
              [],
              [enable_websockets=maybe])

AC_ARG_ENABLE([data-channels],
              [AS_HELP_STRING([--disable-data-channels],
                              [Disable DataChannels])],
              [],
              [enable_data_channels=maybe])

AC_ARG_ENABLE([rabbitmq],
              [AS_HELP_STRING([--disable-rabbitmq],
                              [Disable RabbitMQ integration])],
              [],
              [enable_rabbitmq=maybe])

AC_ARG_ENABLE([mqtt],
              [AS_HELP_STRING([--disable-mqtt],
                              [Disable MQTT integration])],
              [],
              [enable_mqtt=maybe])

AC_ARG_ENABLE([unix-sockets],
              [AS_HELP_STRING([--disable-unix-sockets],
                              [Disable Unix Sockets integration])],
              [],
              [enable_unix_sockets=maybe])

AC_ARG_ENABLE([sample-event-handler],
              [AS_HELP_STRING([--disable-sample-event-handler],
                              [Disable sample event handler (HTTP POST) ])],
              [],
              [enable_sample_event_handler=maybe])

AC_ARG_ENABLE([boringssl],
              [AS_HELP_STRING([--enable-boringssl],
                              [Use BoringSSL instead of OpenSSL])],
              [],
              [enable_boringssl=no])

AC_ARG_ENABLE([libsrtp2],
              [AS_HELP_STRING([--enable-libsrtp2],
                              [Use libsrtp 2.0.x instead of libsrtp 1.5.x])],
              [],
              [enable_libsrtp2=maybe])

AC_ARG_ENABLE([dtls-settimeout],
              [AS_HELP_STRING([--enable-dtls-settimeout],
                              [Use DTLSv1_set_initial_timeout_duration (only available in recent BoringSSL versions)])],
              [],
              [enable_dtls_settimeout=no])

AC_ARG_ENABLE([turn-rest-api],
              [AS_HELP_STRING([--disable-turn-rest-api],
                              [Disable TURN REST API client (via libcurl)])],
              [],
              [enable_turn_rest_api=maybe])

PKG_CHECK_MODULES([JANUS],
                  [
                    glib-2.0 >= $glib_version
                    nice
                    jansson
                    libssl >= $ssl_version
                    libcrypto
                  ])
JANUS_MANUAL_LIBS+=" -lm"
AC_SUBST(JANUS_MANUAL_LIBS)

AS_IF([test "x$enable_boringssl" = "xyes"],
	  [echo "Trying to use BoringSSL instead of OpenSSL...";
	   CFLAGS="$CFLAGS -I/opt/boringssl/include";
	   BORINGSSL_CFLAGS=" -I/opt/boringssl/include";
       AC_SUBST(BORINGSSL_CFLAGS)
	   BORINGSSL_LIBS=" -L/opt/boringssl/lib";
       AC_SUBST(BORINGSSL_LIBS)
	   AC_CHECK_HEADERS([openssl/opensslconf.h],
	                    [],
	                    [AC_MSG_ERROR([BoringSSL headers not found in /opt/boringssl, use --disable-boringssl if you want to use OpenSSL instead])])
      ])
AM_CONDITIONAL([ENABLE_BORINGSSL], [test "x$enable_boringssl" = "xyes"])

AS_IF([test "x$enable_dtls_settimeout" = "xyes"],
      [
      AC_DEFINE(HAVE_DTLS_SETTIMEOUT)
      AC_MSG_NOTICE([Assuming DTLSv1_set_initial_timeout_duration is available])
      ])

AC_CHECK_LIB([nice],
             [nice_agent_set_port_range],
             [AC_DEFINE(HAVE_PORTRANGE)],
             [AC_MSG_NOTICE([libnice version does not have nice_agent_set_port_range])]
             )

AC_CHECK_LIB([nice],
             [nice_address_equal_no_port],
             [AC_DEFINE(HAVE_LIBNICE_TCP)],
             [AC_MSG_NOTICE([libnice version does not support TCP candidates])]
             )

AC_CHECK_LIB([dl],
             [dlopen],
             [JANUS_MANUAL_LIBS+=" -ldl"],
             [AC_MSG_ERROR([libdl not found.])])

AM_CONDITIONAL([ENABLE_LIBSRTP_2], false)
AS_IF([test "x$enable_libsrtp2" != "xno"],
      [AC_CHECK_LIB([srtp2],
                    [srtp_init],
                    [
                      AC_CHECK_HEADER([srtp2/srtp.h],
                                      [
                                        AC_DEFINE(HAVE_SRTP_2)
                                        JANUS_MANUAL_LIBS+=" -lsrtp2"
                                        enable_libsrtp2=yes
                                        AM_CONDITIONAL([ENABLE_LIBSRTP_2], true)
                                      ],
                                      [
                                        AS_IF([test "x$enable_libsrtp2" = "xyes"],
                                              [AC_MSG_ERROR([libsrtp2 headers not found. See README.md for installation instructions or use --disable-libsrtp-2 to try and autodetect libsrtp 1.5.x instead])])
                                      ])
                    ],
                    [
                      AS_IF([test "x$enable_libsrtp2" = "xyes"],
                            [AC_MSG_ERROR([libsrtp2 not found. See README.md for installation instructions or use --disable-libsrtp-2 to try and autodetect libsrtp 1.5.x instead])])
                    ])
      ])
AM_COND_IF([ENABLE_LIBSRTP_2],
           [],
           [AC_CHECK_LIB([srtp],
                         [srtp_init],
                         [
                           AC_CHECK_HEADER([srtp/srtp.h],
                                           [
                                             JANUS_MANUAL_LIBS+=" -lsrtp"
                                             enable_libsrtp2=no
                                           ],
                                           [AC_MSG_ERROR([libsrtp and libsrtp2 headers not found. See README.md for installation instructions])])
                         ],
                         [AC_MSG_ERROR([libsrtp and libsrtp2 not found. See README.md for installation instructions])])
           ])

AC_CHECK_LIB([usrsctp],
             [usrsctp_finish],
             [
               AS_IF([test "x$enable_data_channels" != "xno"],
               [
                  AC_DEFINE(HAVE_SCTP)
                  JANUS_MANUAL_LIBS+=" -lusrsctp"
                  enable_data_channels=yes
               ])
             ],
             [
               AS_IF([test "x$enable_data_channels" = "xyes"],
                     [AC_MSG_ERROR([libusrsctp not found. See README.md for installation instructions or use --disable-data-channels])])
             ])
AM_CONDITIONAL([ENABLE_SCTP], [test "x$enable_data_channels" = "xyes"])

PKG_CHECK_MODULES([LIBCURL],
                  [libcurl],
                  [
                    AS_IF(
                      [test "x$enable_turn_rest_api" != "xno"],
                      [
                        AC_DEFINE(HAVE_LIBCURL)
                        JANUS_MANUAL_LIBS+=" -lcurl"
                        enable_turn_rest_api=yes
                      ])
                    AS_IF([test "x$enable_sample_event_handler" != "xno"],
                      [
                        AC_DEFINE(HAVE_SAMPLEEVH)
                        JANUS_MANUAL_LIBS+=" -lcurl"
                        enable_sample_event_handler=yes
                      ])
                  ],
                  [
                    AS_IF([test "x$enable_turn_rest_api" = "xyes"],
                          [AC_MSG_ERROR([libcurl not found not found. See README.md for installation instructions or use --disable-turn-rest-api])])
                    AS_IF([test "x$enable_sample_event_handler" = "xyes"],
                          [AC_MSG_ERROR([libcurl not found not found. See README.md for installation instructions or use --disable-sample-event-handler])])
                  ])
AM_CONDITIONAL([ENABLE_TURN_REST_API], [test "x$enable_turn_rest_api" = "xyes"])
AM_CONDITIONAL([ENABLE_SAMPLEEVH], [test "x$enable_sample_event_handler" = "xyes"])

AC_CHECK_PROG([DOXYGEN],
              [doxygen],
              [doxygen])
AC_CHECK_PROG([DOT],
              [dot],
              [dot])
AS_IF([test -z "$DOXYGEN" -o -z "$DOT"],
      [
        AS_IF([test "x$enable_docs" = "xyes"],
              [AC_MSG_ERROR([doxygen or dot not found. See README.md for installation instructions or remove --enable-docs])])
      ])
AM_CONDITIONAL([ENABLE_DOCS], [test "x$enable_docs" = "xyes"])

AC_CHECK_PROG([GENGETOPT_CHECK],
              [gengetopt],
              [yes])
if test x"$GENGETOPT_CHECK" != x"yes" ; then
    AC_MSG_ERROR([Please install gengetopt before installing.])
fi


##
# Transports
##

PKG_CHECK_MODULES([TRANSPORTS],
                  [
                    glib-2.0 >= $glib_version
                    jansson
                  ])

PKG_CHECK_MODULES([MHD],
                  [libmicrohttpd],
                  [
                    AS_IF([test "x$enable_rest" = "xmaybe"],
                          [enable_rest=yes])
                  ],
                  [
                    AS_IF([test "x$enable_rest" = "xyes"],
                          [AC_MSG_ERROR([libmicrohttpd not found. See README.md for installation instructions or use --disable-rest])])
                  ])
AC_SUBST([MHD_CFLAGS])
AC_SUBST([MHD_LIBS])
AM_CONDITIONAL([ENABLE_REST], [test "x$enable_rest" = "xyes"])

AM_CONDITIONAL([ENABLE_WEBSOCKETS_NEWAPI], false)
AC_CHECK_LIB([websockets],
             [libwebsocket_create_context],
             [
               AS_IF([test "x$enable_websockets" != "xno"],
               [
                  AC_DEFINE(HAVE_WEBSOCKETS)
                  WS_MANUAL_LIBS="-lwebsockets"
                  enable_websockets=yes
               ])
             ],
             [
				AC_CHECK_LIB([websockets],
							 [lws_create_context],
							 [
							   AS_IF([test "x$enable_websockets" != "xno"],
							   [
								  AC_DEFINE(HAVE_WEBSOCKETS)
								  AC_DEFINE(HAVE_LIBWEBSOCKETS_NEWAPI)
								  AM_CONDITIONAL([ENABLE_WEBSOCKETS_NEWAPI], true)
								  WS_MANUAL_LIBS="-lwebsockets"
								  enable_websockets=yes
							   ])
							 ],
							 [
							   AS_IF([test "x$enable_websockets" = "xyes"],
									 [AC_MSG_ERROR([libwebsockets not found. See README.md for installation instructions or use --disable-websockets])])
							 ])
             ])
AM_CONDITIONAL([ENABLE_WEBSOCKETS], [test "x$enable_websockets" = "xyes"])
AC_SUBST(WS_MANUAL_LIBS)

AC_CHECK_LIB([rabbitmq],
             [amqp_error_string2],
             [
               AS_IF([test "x$enable_rabbitmq" != "xno"],
               [
                  AC_DEFINE(HAVE_RABBITMQ)
                  enable_rabbitmq=yes
               ])
             ],
             [
               AS_IF([test "x$enable_rabbitmq" = "xyes"],
                     [AC_MSG_ERROR([rabbitmq-c not found. See README.md for installation instructions or use --disable-rabbitmq])])
             ])
AC_CHECK_LIB([paho-mqtt3a],
             [MQTTAsync_create],
             [
               AS_IF([test "x$enable_mqtt" != "xno"],
               [
                  AC_DEFINE(HAVE_MQTT)
                  enable_mqtt=yes
               ])
             ],
             [
               AS_IF([test "x$enable_mqtt" = "xyes"],
                     [AC_MSG_ERROR([paho c client not found. See README.md for installation instructions or use --disable-mqtt])])
             ])
AM_CONDITIONAL([ENABLE_RABBITMQ], [test "x$enable_rabbitmq" = "xyes"])
AM_CONDITIONAL([ENABLE_MQTT], [test "x$enable_mqtt" = "xyes"])

AC_TRY_COMPILE([
               #include <stdlib.h>
               #include <sys/socket.h>
               #include <sys/un.h>
               void test() {
                 int pfd = socket(PF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
                 if(pfd < 0)
                   exit(1);
               }],
               [],
               [
                 AS_IF([test "x$enable_unix_sockets" != "xno"],
                 [
                    AC_DEFINE(HAVE_PFUNIX)
                    enable_unix_sockets=yes
                 ])
               ],
               [
                 AS_IF([test "x$enable_unix_sockets" = "xyes"],
                       [AC_MSG_ERROR([SOCK_SEQPACKET not defined in your OS. Use --disable-unix-sockets])])
               ])
AM_CONDITIONAL([ENABLE_PFUNIX], [test "x$enable_unix_sockets" = "xyes"])


##
# Plugins
##
JANUS_MANUAL_LIBS+= "-lavutil -lavfilter -lswresample -lswscale -lavdevice $(pkg-config --cflags --libs libavformat libavcodec) -lpostproc -lavutil
 `pkg-config --cflags --libs glib-2.0` `pkg-config --cflags --libs opus` `pkg-config --cflags --libs ogg` `pkg-config --cflags --libs opencv`"
PKG_CHECK_MODULES([PLUGINS],
                  [
                    glib-2.0 >= $glib_version
                    libavfilter,
		    libswresample,
		    libswscale,
		    libavdevice,
		    libavutil,
		    libavformat,
		    libavcodec,
		    libpostproc,
		    opus,
		    ogg,
		    jansson
                  ])

AC_ARG_ENABLE([plugin-audiobridge],
              [AS_HELP_STRING([--disable-plugin-audiobridge],
                              [Disable audiobridge plugin])],
              [],
              [enable_plugin_audiobridge=maybe])

AC_ARG_ENABLE([plugin-echotest],
              [AS_HELP_STRING([--disable-plugin-echotest],
                              [Disable echotest plugin])],
              [],
              [enable_plugin_echotest=yes])

AC_ARG_ENABLE([plugin-recordplay],
              [AS_HELP_STRING([--disable-plugin-recordplay],
                              [Disable record&play plugin])],
              [],
              [enable_plugin_recordplay=yes])

AC_ARG_ENABLE([plugin-sip],
              [AS_HELP_STRING([--disable-plugin-sip],
                              [Disable sip plugin])],
              [],
              [enable_plugin_sip=maybe])

AC_ARG_ENABLE([plugin-streaming],
              [AS_HELP_STRING([--disable-plugin-streaming],
                              [Disable streaming plugin])],
              [],
              [enable_plugin_streaming=yes])

AC_ARG_ENABLE([plugin-videocall],
              [AS_HELP_STRING([--disable-plugin-videocall],
                              [Disable videocall plugin])],
              [],
              [enable_plugin_videocall=yes])

AC_ARG_ENABLE([plugin-videoroom],
              [AS_HELP_STRING([--disable-plugin-videoroom],
                              [Disable videoroom plugin])],
              [],
              [enable_plugin_videoroom=yes])

AC_ARG_ENABLE([plugin-voicemail],
              [AS_HELP_STRING([--disable-plugin-voicemail],
                              [Disable voicemail plugin])],
              [],
              [enable_plugin_voicemail=maybe])

AC_ARG_ENABLE([plugin-textroom],
              [AS_HELP_STRING([--disable-plugin-textroom],
                              [Disable textroom plugin])],
              [],
              [enable_plugin_textroom=yes])

PKG_CHECK_MODULES([SOFIA],
                  [sofia-sip-ua],
                  [
                    AS_IF([test "x$enable_plugin_sip" = "xmaybe"],
                          [enable_plugin_sip=yes])
                  ],
                  [
                    AS_IF([test "x$enable_plugin_sip" = "xyes"],
                          [AC_MSG_ERROR([sofia-sip-ua not found not found. See README.md for installation instructions or use --disable-plugin-sip])])
                  ])
AC_SUBST([SOFIA_CFLAGS])
AC_SUBST([SOFIA_LIBS])

PKG_CHECK_MODULES([LIBAVFORMAT],
                 [libavformat],
                  [
                    AS_IF([test "x$enable_plugin_echotest" = "xmaybe"],
                          [enable_plugin_echotest=yes])
                  ],
                  [
                    AS_IF([test "x$enable_plugin_echotest" = "xyes"],
                          [AC_MSG_ERROR([sofia-sip-ua not found not found. See README.md for installation instructions or use --disable-plugin-sip])])
                  ])
AC_SUBST([LIBAVFORMAT_CFLAGS])
AC_SUBST([LIBAVFORMAT_LIBS])
#AC_SUBST([LIBAVCODEC_CFLAGS])
#AC_SUBST([LIBAVCODEC_LIBS])


PKG_CHECK_MODULES([OPUS],
                  [opus],
                  [
                    AS_IF([test "x$enable_plugin_audiobridge" = "xmaybe"],
                          [enable_plugin_audiobridge=yes])
                  ],
                  [
                    AS_IF([test "x$enable_plugin_audiobridge" = "xyes"],
                          [AC_MSG_ERROR([libopus not found not found. See README.md for installation instructions or use --disable-plugin-audiobridge])])
                  ])
AC_SUBST([OPUS_CFLAGS])
AC_SUBST([OPUS_LIBS])

PKG_CHECK_MODULES([OGG],
                  [ogg],
                  [
                    AS_IF([test "x$enable_plugin_voicemail" = "xmaybe"],
                          [enable_plugin_voicemail=yes])
                  ],
                  [
                    AS_IF([test "x$enable_plugin_voicemail" = "xyes"],
                          [AC_MSG_ERROR([libogg not found not found. See README.md for installation instructions or use --disable-plugin-voicemail])])
                  ])
AC_SUBST([OGG_CFLAGS])
AC_SUBST([OGG_LIBS])

AM_CONDITIONAL([ENABLE_PLUGIN_AUDIOBRIDGE], [test "x$enable_plugin_audiobridge" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_ECHOTEST], [test "x$enable_plugin_echotest" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_RECORDPLAY], [test "x$enable_plugin_recordplay" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_SIP], [test "x$enable_plugin_sip" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_STREAMING], [test "x$enable_plugin_streaming" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_VIDEOCALL], [test "x$enable_plugin_videocall" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_VIDEOROOM], [test "x$enable_plugin_videoroom" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_VOICEMAIL], [test "x$enable_plugin_voicemail" = "xyes"])
AM_CONDITIONAL([ENABLE_PLUGIN_TEXTROOM], [test "x$enable_plugin_textroom" = "xyes"])


##
# Event handlers
##

PKG_CHECK_MODULES([EVENTS],
                  [
                    glib-2.0 >= $glib_version
                    jansson
                  ])


##
# Post-processing
##

AC_ARG_ENABLE([post-processing],
              [AS_HELP_STRING([--enable-post-processing],
                              [Enable building post-processing utility])],
              [],
              [enable_post_processing=no])

AS_IF([test "x$enable_post_processing" = "xyes"],
      [PKG_CHECK_MODULES([POST_PROCESSING],
                         [
                           glib-2.0,
                           jansson,
                           libavutil,
                           libavcodec,
			   libavformat,
                           ogg
                         ])
      ])

AM_CONDITIONAL([ENABLE_POST_PROCESSING], [test "x$enable_post_processing" = "xyes"])

AC_CONFIG_FILES([
  Makefile
  html/Makefile
  docs/Makefile
])


#JANUS_MANUAL_LIBS+= "-lavfilter -lswresample -lswscale -lavdevice $(pkg-config --cflags --libs libavformat libavcodec) -lpostproc
# `pkg-config --cflags --libs glib-2.0` `pkg-config --cflags --libs opus` `pkg-config --cflags --libs ogg`"
JANUS_MANUAL_LIBS+=" -pthread"

AC_OUTPUT

##
# Summary
##
echo

AM_COND_IF([ENABLE_LIBSRTP_2],
	[echo "libsrtp version:           2.0.x"],
	[echo "libsrtp version:           1.5.x"])
AM_COND_IF([ENABLE_BORINGSSL],
	[echo "SSL/crypto library:        BoringSSL"],
	[echo "SSL/crypto library:        OpenSSL"])
AM_COND_IF([ENABLE_SCTP],
	[echo "DataChannels support:      yes"],
	[echo "DataChannels support:      no"])
AM_COND_IF([ENABLE_POST_PROCESSING],
	[echo "Recordings post-processor: yes"],
	[echo "Recordings post-processor: no"])
AM_COND_IF([ENABLE_TURN_REST_API],
	[echo "TURN REST API client:      yes"],
	[echo "TURN REST API client:      no"])
AM_COND_IF([ENABLE_DOCS],
	[echo "Doxygen documentation:     yes"],
	[echo "Doxygen documentation:     no"])
echo "Transports:"
AM_COND_IF([ENABLE_REST],
	[echo "    REST (HTTP/HTTPS):     yes"],
	[echo "    REST (HTTP/HTTPS):     no"])
AM_COND_IF([ENABLE_WEBSOCKETS],
	[
     AM_COND_IF([ENABLE_WEBSOCKETS_NEWAPI],
		[echo "    WebSockets:            yes (new API)"],
		[echo "    WebSockets:            yes (old API)"])
	],
	[echo "    WebSockets:            no"])
AM_COND_IF([ENABLE_RABBITMQ],
	[echo "    RabbitMQ:              yes"],
	[echo "    RabbitMQ:              no"])
AM_COND_IF([ENABLE_MQTT],
	[echo "    MQTT:                  yes"],
	[echo "    MQTT:                  no"])
AM_COND_IF([ENABLE_PFUNIX],
	[echo "    Unix Sockets:          yes"],
	[echo "    Unix Sockets:          no"])
echo "Plugins:"
AM_COND_IF([ENABLE_PLUGIN_ECHOTEST],
	[echo "    Echo Test:             yes"],
	[echo "    Echo Test:             no"])
AM_COND_IF([ENABLE_PLUGIN_STREAMING],
	[echo "    Streaming:             yes"],
	[echo "    Streaming:             no"])
AM_COND_IF([ENABLE_PLUGIN_VIDEOCALL],
	[echo "    Video Call:            yes"],
	[echo "    Video Call:            no"])
AM_COND_IF([ENABLE_PLUGIN_SIP],
	[echo "    SIP Gateway:           yes"],
	[echo "    SIP Gateway:           no"])
AM_COND_IF([ENABLE_PLUGIN_AUDIOBRIDGE],
	[echo "    Audio Bridge:          yes"],
	[echo "    Audio Bridge:          no"])
AM_COND_IF([ENABLE_PLUGIN_VIDEOROOM],
	[echo "    Video Room:            yes"],
	[echo "    Video Room:            no"])
AM_COND_IF([ENABLE_PLUGIN_VOICEMAIL],
	[echo "    Voice Mail:            yes"],
	[echo "    Voice Mail:            no"])
AM_COND_IF([ENABLE_PLUGIN_RECORDPLAY],
	[echo "    Record&Play:           yes"],
	[echo "    Record&Play:           no"])
AM_COND_IF([ENABLE_PLUGIN_TEXTROOM],
	[echo "    Text Room:             yes"],
	[echo "    Text Room:             no"])
echo "Event handlers:"
AM_COND_IF([ENABLE_SAMPLEEVH],
	[echo "    Sample event handler:  yes"],
	[echo "    Sample event handler:  no"])

echo
echo "If this configuration is ok for you, do a 'make' to start building Janus. A 'make install' will install Janus and its plugins to the specified prefix. Finally, a 'make configs' will install some sample configuration files too (something you'll only want to do the first time, though)."
echo
