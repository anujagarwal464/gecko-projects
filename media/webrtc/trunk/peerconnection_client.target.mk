# This file is generated by gyp; do not edit.

TOOLSET := target
TARGET := peerconnection_client
DEFS_Debug := \
	'-DWEBRTC_SVNREVISION="Unavailable_issue687"' \
	'-D_FILE_OFFSET_BITS=64' \
	'-DCHROMIUM_BUILD' \
	'-DUSE_LIBJPEG_TURBO=1' \
	'-DENABLE_ONE_CLICK_SIGNIN' \
	'-DGTK_DISABLE_SINGLE_INCLUDES=1' \
	'-DENABLE_REMOTING=1' \
	'-DENABLE_WEBRTC=1' \
	'-DENABLE_CONFIGURATION_POLICY' \
	'-DENABLE_INPUT_SPEECH' \
	'-DENABLE_NOTIFICATIONS' \
	'-DENABLE_GPU=1' \
	'-DUSE_OPENSSL=1' \
	'-DENABLE_EGLIMAGE=1' \
	'-DUSE_SKIA=1' \
	'-DENABLE_TASK_MANAGER=1' \
	'-DENABLE_WEB_INTENTS=1' \
	'-DENABLE_EXTENSIONS=1' \
	'-DENABLE_PLUGIN_INSTALLATION=1' \
	'-DENABLE_PROTECTOR_SERVICE=1' \
	'-DENABLE_SESSION_SERVICE=1' \
	'-DENABLE_THEMES=1' \
	'-DENABLE_BACKGROUND=1' \
	'-DENABLE_AUTOMATION=1' \
	'-DENABLE_PRINTING=1' \
	'-DENABLE_CAPTIVE_PORTAL_DETECTION=1' \
	'-DWEBRTC_CHROMIUM_BUILD' \
	'-DWEBRTC_LINUX' \
	'-DWEBRTC_THREAD_RR' \
	'-DFEATURE_ENABLE_SSL' \
	'-DFEATURE_ENABLE_VOICEMAIL' \
	'-DEXPAT_RELATIVE_PATH' \
	'-DGTEST_RELATIVE_PATH' \
	'-DJSONCPP_RELATIVE_PATH' \
	'-DNO_MAIN_THREAD_WRAPPING' \
	'-DNO_SOUND_SYSTEM' \
	'-DLINUX' \
	'-DPOSIX' \
	'-D__STDC_FORMAT_MACROS' \
	'-DDYNAMIC_ANNOTATIONS_ENABLED=1' \
	'-DWTF_USE_DYNAMIC_ANNOTATIONS=1' \
	'-D_DEBUG'

# Flags passed to all source files.
CFLAGS_Debug := \
	-Werror \
	-pthread \
	-fno-exceptions \
	-fno-strict-aliasing \
	-Wall \
	-Wno-unused-parameter \
	-Wno-missing-field-initializers \
	-fvisibility=hidden \
	-pipe \
	-fPIC \
	-pthread \
	-I/usr/include/gtk-2.0 \
	-I/usr/lib64/gtk-2.0/include \
	-I/usr/include/atk-1.0 \
	-I/usr/include/cairo \
	-I/usr/include/gdk-pixbuf-2.0 \
	-I/usr/include/pango-1.0 \
	-I/usr/include/glib-2.0 \
	-I/usr/lib64/glib-2.0/include \
	-I/usr/include/pixman-1 \
	-I/usr/include/freetype2 \
	-I/usr/include/libpng15 \
	-O0 \
	-g

# Flags passed to only C files.
CFLAGS_C_Debug :=

# Flags passed to only C++ files.
CFLAGS_CC_Debug := \
	-fno-rtti \
	-fno-threadsafe-statics \
	-fvisibility-inlines-hidden \
	-Wsign-compare

INCS_Debug := \
	-Ithird_party/webrtc \
	-Ithird_party \
	-Ithird_party/libjingle/source \
	-Ithird_party/jsoncpp/overrides/include \
	-Ithird_party/jsoncpp/source/include \
	-Ithird_party/libjingle/overrides \
	-Itesting/gtest/include

DEFS_Release := \
	'-DWEBRTC_SVNREVISION="Unavailable_issue687"' \
	'-D_FILE_OFFSET_BITS=64' \
	'-DCHROMIUM_BUILD' \
	'-DUSE_LIBJPEG_TURBO=1' \
	'-DENABLE_ONE_CLICK_SIGNIN' \
	'-DGTK_DISABLE_SINGLE_INCLUDES=1' \
	'-DENABLE_REMOTING=1' \
	'-DENABLE_WEBRTC=1' \
	'-DENABLE_CONFIGURATION_POLICY' \
	'-DENABLE_INPUT_SPEECH' \
	'-DENABLE_NOTIFICATIONS' \
	'-DENABLE_GPU=1' \
	'-DUSE_OPENSSL=1' \
	'-DENABLE_EGLIMAGE=1' \
	'-DUSE_SKIA=1' \
	'-DENABLE_TASK_MANAGER=1' \
	'-DENABLE_WEB_INTENTS=1' \
	'-DENABLE_EXTENSIONS=1' \
	'-DENABLE_PLUGIN_INSTALLATION=1' \
	'-DENABLE_PROTECTOR_SERVICE=1' \
	'-DENABLE_SESSION_SERVICE=1' \
	'-DENABLE_THEMES=1' \
	'-DENABLE_BACKGROUND=1' \
	'-DENABLE_AUTOMATION=1' \
	'-DENABLE_PRINTING=1' \
	'-DENABLE_CAPTIVE_PORTAL_DETECTION=1' \
	'-DWEBRTC_CHROMIUM_BUILD' \
	'-DWEBRTC_LINUX' \
	'-DWEBRTC_THREAD_RR' \
	'-DFEATURE_ENABLE_SSL' \
	'-DFEATURE_ENABLE_VOICEMAIL' \
	'-DEXPAT_RELATIVE_PATH' \
	'-DGTEST_RELATIVE_PATH' \
	'-DJSONCPP_RELATIVE_PATH' \
	'-DNO_MAIN_THREAD_WRAPPING' \
	'-DNO_SOUND_SYSTEM' \
	'-DLINUX' \
	'-DPOSIX' \
	'-D__STDC_FORMAT_MACROS' \
	'-DNDEBUG' \
	'-DNVALGRIND' \
	'-DDYNAMIC_ANNOTATIONS_ENABLED=0'

# Flags passed to all source files.
CFLAGS_Release := \
	-Werror \
	-pthread \
	-fno-exceptions \
	-fno-strict-aliasing \
	-Wall \
	-Wno-unused-parameter \
	-Wno-missing-field-initializers \
	-fvisibility=hidden \
	-pipe \
	-fPIC \
	-pthread \
	-I/usr/include/gtk-2.0 \
	-I/usr/lib64/gtk-2.0/include \
	-I/usr/include/atk-1.0 \
	-I/usr/include/cairo \
	-I/usr/include/gdk-pixbuf-2.0 \
	-I/usr/include/pango-1.0 \
	-I/usr/include/glib-2.0 \
	-I/usr/lib64/glib-2.0/include \
	-I/usr/include/pixman-1 \
	-I/usr/include/freetype2 \
	-I/usr/include/libpng15 \
	-O2 \
	-fno-ident \
	-fdata-sections \
	-ffunction-sections

# Flags passed to only C files.
CFLAGS_C_Release :=

# Flags passed to only C++ files.
CFLAGS_CC_Release := \
	-fno-rtti \
	-fno-threadsafe-statics \
	-fvisibility-inlines-hidden \
	-Wsign-compare

INCS_Release := \
	-Ithird_party/webrtc \
	-Ithird_party \
	-Ithird_party/libjingle/source \
	-Ithird_party/jsoncpp/overrides/include \
	-Ithird_party/jsoncpp/source/include \
	-Ithird_party/libjingle/overrides \
	-Itesting/gtest/include

OBJS := \
	$(obj).target/$(TARGET)/third_party/libjingle/source/talk/examples/peerconnection/client/conductor.o \
	$(obj).target/$(TARGET)/third_party/libjingle/source/talk/examples/peerconnection/client/defaults.o \
	$(obj).target/$(TARGET)/third_party/libjingle/source/talk/examples/peerconnection/client/linux/main.o \
	$(obj).target/$(TARGET)/third_party/libjingle/source/talk/examples/peerconnection/client/linux/main_wnd.o \
	$(obj).target/$(TARGET)/third_party/libjingle/source/talk/examples/peerconnection/client/peer_connection_client.o

# Add to the list of files we specially track dependencies for.
all_deps += $(OBJS)

# Make sure our dependencies are built before any of us.
$(OBJS): | $(obj).target/third_party/jsoncpp/libjsoncpp.a $(obj).target/third_party/libjingle/libjingle_peerconnection.a $(obj).target/base/base.stamp $(obj).target/net/net.stamp $(obj).target/third_party/expat/expat.stamp $(obj).target/third_party/libsrtp/libsrtp.a $(obj).target/third_party/webrtc/modules/libvideo_capture_module.a $(obj).target/third_party/webrtc/modules/libwebrtc_utility.a $(obj).target/third_party/webrtc/modules/libaudio_coding_module.a $(obj).target/third_party/webrtc/modules/libCNG.a $(obj).target/third_party/webrtc/common_audio/libsignal_processing.a $(obj).target/third_party/webrtc/system_wrappers/source/libsystem_wrappers.a $(obj).target/third_party/webrtc/modules/libG711.a $(obj).target/third_party/webrtc/modules/libG722.a $(obj).target/third_party/webrtc/modules/libiLBC.a $(obj).target/third_party/webrtc/modules/libiSAC.a $(obj).target/third_party/webrtc/modules/libiSACFix.a $(obj).target/third_party/webrtc/modules/libPCM16B.a $(obj).target/third_party/webrtc/modules/libNetEq.a $(obj).target/third_party/webrtc/common_audio/libresampler.a $(obj).target/third_party/webrtc/common_audio/libvad.a $(obj).target/third_party/webrtc/modules/libwebrtc_video_coding.a $(obj).target/third_party/webrtc/modules/libwebrtc_i420.a $(obj).target/third_party/webrtc/common_video/libcommon_video.a $(obj).target/third_party/libjpeg_turbo/libjpeg_turbo.a $(obj).target/third_party/libyuv/libyuv.a $(obj).target/third_party/webrtc/modules/video_coding/codecs/vp8/libwebrtc_vp8.a $(obj).target/third_party/libvpx/libvpx.a $(obj).target/third_party/libvpx/gen_asm_offsets.stamp $(obj).target/third_party/libvpx/libvpx_asm_offsets.a $(obj).target/third_party/webrtc/modules/libvideo_render_module.a $(obj).target/third_party/webrtc/video_engine/libvideo_engine_core.a $(obj).target/third_party/webrtc/modules/libmedia_file.a $(obj).target/third_party/webrtc/modules/librtp_rtcp.a $(obj).target/third_party/webrtc/modules/libremote_bitrate_estimator.a $(obj).target/third_party/webrtc/modules/libudp_transport.a $(obj).target/third_party/webrtc/modules/libbitrate_controller.a $(obj).target/third_party/webrtc/modules/libvideo_processing.a $(obj).target/third_party/webrtc/modules/libvideo_processing_sse2.a $(obj).target/third_party/webrtc/voice_engine/libvoice_engine_core.a $(obj).target/third_party/webrtc/modules/libaudio_conference_mixer.a $(obj).target/third_party/webrtc/modules/libaudio_processing.a $(obj).target/third_party/webrtc/modules/libaudioproc_debug_proto.a $(obj).target/third_party/protobuf/libprotobuf_lite.a $(obj).target/third_party/webrtc/modules/libaudio_processing_sse2.a $(obj).target/third_party/webrtc/modules/libaudio_device.a $(obj).target/third_party/libjingle/libjingle.a $(obj).target/third_party/libjingle/libjingle_p2p.a

# CFLAGS et al overrides must be target-local.
# See "Target-specific Variable Values" in the GNU Make manual.
$(OBJS): TOOLSET := $(TOOLSET)
$(OBJS): GYP_CFLAGS := $(DEFS_$(BUILDTYPE)) $(INCS_$(BUILDTYPE))  $(CFLAGS_$(BUILDTYPE)) $(CFLAGS_C_$(BUILDTYPE))
$(OBJS): GYP_CXXFLAGS := $(DEFS_$(BUILDTYPE)) $(INCS_$(BUILDTYPE))  $(CFLAGS_$(BUILDTYPE)) $(CFLAGS_CC_$(BUILDTYPE))

# Suffix rules, putting all outputs into $(obj).

$(obj).$(TOOLSET)/$(TARGET)/%.o: $(srcdir)/%.cc FORCE_DO_CMD
	@$(call do_cmd,cxx,1)

# Try building from generated source, too.

$(obj).$(TOOLSET)/$(TARGET)/%.o: $(obj).$(TOOLSET)/%.cc FORCE_DO_CMD
	@$(call do_cmd,cxx,1)

$(obj).$(TOOLSET)/$(TARGET)/%.o: $(obj)/%.cc FORCE_DO_CMD
	@$(call do_cmd,cxx,1)

# End of this set of suffix rules
### Rules for final target.
LDFLAGS_Debug := \
	-pthread \
	-Wl,-z,noexecstack \
	-fPIC \
	-Wl,--threads \
	-Wl,--thread-count=4 \
	-B$(builddir)/../../third_party/gold \
	-Wl,--icf=none

LDFLAGS_Release := \
	-pthread \
	-Wl,-z,noexecstack \
	-fPIC \
	-Wl,--threads \
	-Wl,--thread-count=4 \
	-B$(builddir)/../../third_party/gold \
	-Wl,--icf=none \
	-Wl,-O1 \
	-Wl,--as-needed \
	-Wl,--gc-sections

LIBS := \
	 \
	-lgtk-x11-2.0 \
	-lgdk-x11-2.0 \
	-latk-1.0 \
	-lgio-2.0 \
	-lpangoft2-1.0 \
	-lpangocairo-1.0 \
	-lgdk_pixbuf-2.0 \
	-lcairo \
	-lpango-1.0 \
	-lfreetype \
	-lfontconfig \
	-lgobject-2.0 \
	-lgthread-2.0 \
	-lrt \
	-lglib-2.0 \
	-lX11 \
	-lXcomposite \
	-lXext \
	-lXrender \
	-lexpat \
	-ldl

$(builddir)/peerconnection_client: GYP_LDFLAGS := $(LDFLAGS_$(BUILDTYPE))
$(builddir)/peerconnection_client: LIBS := $(LIBS)
$(builddir)/peerconnection_client: LD_INPUTS := $(OBJS) $(obj).target/third_party/jsoncpp/libjsoncpp.a $(obj).target/third_party/libjingle/libjingle_peerconnection.a $(obj).target/third_party/libsrtp/libsrtp.a $(obj).target/third_party/webrtc/modules/libvideo_capture_module.a $(obj).target/third_party/webrtc/modules/libwebrtc_utility.a $(obj).target/third_party/webrtc/modules/libaudio_coding_module.a $(obj).target/third_party/webrtc/modules/libCNG.a $(obj).target/third_party/webrtc/common_audio/libsignal_processing.a $(obj).target/third_party/webrtc/system_wrappers/source/libsystem_wrappers.a $(obj).target/third_party/webrtc/modules/libG711.a $(obj).target/third_party/webrtc/modules/libG722.a $(obj).target/third_party/webrtc/modules/libiLBC.a $(obj).target/third_party/webrtc/modules/libiSAC.a $(obj).target/third_party/webrtc/modules/libiSACFix.a $(obj).target/third_party/webrtc/modules/libPCM16B.a $(obj).target/third_party/webrtc/modules/libNetEq.a $(obj).target/third_party/webrtc/common_audio/libresampler.a $(obj).target/third_party/webrtc/common_audio/libvad.a $(obj).target/third_party/webrtc/modules/libwebrtc_video_coding.a $(obj).target/third_party/webrtc/modules/libwebrtc_i420.a $(obj).target/third_party/webrtc/common_video/libcommon_video.a $(obj).target/third_party/libjpeg_turbo/libjpeg_turbo.a $(obj).target/third_party/libyuv/libyuv.a $(obj).target/third_party/webrtc/modules/video_coding/codecs/vp8/libwebrtc_vp8.a $(obj).target/third_party/libvpx/libvpx.a $(obj).target/third_party/libvpx/libvpx_asm_offsets.a $(obj).target/third_party/webrtc/modules/libvideo_render_module.a $(obj).target/third_party/webrtc/video_engine/libvideo_engine_core.a $(obj).target/third_party/webrtc/modules/libmedia_file.a $(obj).target/third_party/webrtc/modules/librtp_rtcp.a $(obj).target/third_party/webrtc/modules/libremote_bitrate_estimator.a $(obj).target/third_party/webrtc/modules/libudp_transport.a $(obj).target/third_party/webrtc/modules/libbitrate_controller.a $(obj).target/third_party/webrtc/modules/libvideo_processing.a $(obj).target/third_party/webrtc/modules/libvideo_processing_sse2.a $(obj).target/third_party/webrtc/voice_engine/libvoice_engine_core.a $(obj).target/third_party/webrtc/modules/libaudio_conference_mixer.a $(obj).target/third_party/webrtc/modules/libaudio_processing.a $(obj).target/third_party/webrtc/modules/libaudioproc_debug_proto.a $(obj).target/third_party/protobuf/libprotobuf_lite.a $(obj).target/third_party/webrtc/modules/libaudio_processing_sse2.a $(obj).target/third_party/webrtc/modules/libaudio_device.a $(obj).target/third_party/libjingle/libjingle.a $(obj).target/third_party/libjingle/libjingle_p2p.a
$(builddir)/peerconnection_client: TOOLSET := $(TOOLSET)
$(builddir)/peerconnection_client: $(OBJS) $(obj).target/third_party/jsoncpp/libjsoncpp.a $(obj).target/third_party/libjingle/libjingle_peerconnection.a $(obj).target/third_party/libsrtp/libsrtp.a $(obj).target/third_party/webrtc/modules/libvideo_capture_module.a $(obj).target/third_party/webrtc/modules/libwebrtc_utility.a $(obj).target/third_party/webrtc/modules/libaudio_coding_module.a $(obj).target/third_party/webrtc/modules/libCNG.a $(obj).target/third_party/webrtc/common_audio/libsignal_processing.a $(obj).target/third_party/webrtc/system_wrappers/source/libsystem_wrappers.a $(obj).target/third_party/webrtc/modules/libG711.a $(obj).target/third_party/webrtc/modules/libG722.a $(obj).target/third_party/webrtc/modules/libiLBC.a $(obj).target/third_party/webrtc/modules/libiSAC.a $(obj).target/third_party/webrtc/modules/libiSACFix.a $(obj).target/third_party/webrtc/modules/libPCM16B.a $(obj).target/third_party/webrtc/modules/libNetEq.a $(obj).target/third_party/webrtc/common_audio/libresampler.a $(obj).target/third_party/webrtc/common_audio/libvad.a $(obj).target/third_party/webrtc/modules/libwebrtc_video_coding.a $(obj).target/third_party/webrtc/modules/libwebrtc_i420.a $(obj).target/third_party/webrtc/common_video/libcommon_video.a $(obj).target/third_party/libjpeg_turbo/libjpeg_turbo.a $(obj).target/third_party/libyuv/libyuv.a $(obj).target/third_party/webrtc/modules/video_coding/codecs/vp8/libwebrtc_vp8.a $(obj).target/third_party/libvpx/libvpx.a $(obj).target/third_party/libvpx/libvpx_asm_offsets.a $(obj).target/third_party/webrtc/modules/libvideo_render_module.a $(obj).target/third_party/webrtc/video_engine/libvideo_engine_core.a $(obj).target/third_party/webrtc/modules/libmedia_file.a $(obj).target/third_party/webrtc/modules/librtp_rtcp.a $(obj).target/third_party/webrtc/modules/libremote_bitrate_estimator.a $(obj).target/third_party/webrtc/modules/libudp_transport.a $(obj).target/third_party/webrtc/modules/libbitrate_controller.a $(obj).target/third_party/webrtc/modules/libvideo_processing.a $(obj).target/third_party/webrtc/modules/libvideo_processing_sse2.a $(obj).target/third_party/webrtc/voice_engine/libvoice_engine_core.a $(obj).target/third_party/webrtc/modules/libaudio_conference_mixer.a $(obj).target/third_party/webrtc/modules/libaudio_processing.a $(obj).target/third_party/webrtc/modules/libaudioproc_debug_proto.a $(obj).target/third_party/protobuf/libprotobuf_lite.a $(obj).target/third_party/webrtc/modules/libaudio_processing_sse2.a $(obj).target/third_party/webrtc/modules/libaudio_device.a $(obj).target/third_party/libjingle/libjingle.a $(obj).target/third_party/libjingle/libjingle_p2p.a FORCE_DO_CMD
	$(call do_cmd,link)

all_deps += $(builddir)/peerconnection_client
# Add target alias
.PHONY: peerconnection_client
peerconnection_client: $(builddir)/peerconnection_client

# Add executable to "all" target.
.PHONY: all
all: $(builddir)/peerconnection_client

