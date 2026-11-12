// SPDX-License-Identifier: Apache-2.0
//
// The base player's agent tools (see AgentTools.h). Descriptions are agent-facing API docs:
// state what the tool does, the units, and the preconditions. Schemas are JSON Schema
// objects; mark required args.
#include "AgentTools.h"

namespace mp {

void RegisterPlayerAgentTools(const AgentToolRegisterFn& reg) {
    reg(
        "play_pause",
        "Toggle play/pause of the current video. If the clip already played to the end, "
        "restarts it from the beginning. No-op for still images. Returns the new playing "
        "state and the current position in seconds.",
        "{\"type\":\"object\"}");

    reg(
        "open_file",
        "Open a media file by path, replacing whatever is loaded. Supports SBS images "
        "(jpg/png), LIF containers, and SBS video (mp4/mkv/mov). Returns the loaded file "
        "and its kind on success; an error result if the path cannot be opened.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path to the media file to open (absolute recommended).\"}},"
        "\"required\":[\"path\"]}");

    reg(
        "seek",
        "Seek the current video to an absolute position in seconds, clamped to "
        "[0, duration]. Requires a video to be open (errors for still images). Returns the "
        "applied position in seconds.",
        "{\"type\":\"object\",\"properties\":{\"position_s\":{\"type\":\"number\","
        "\"minimum\":0,\"description\":\"Target position in seconds from the start.\"}},"
        "\"required\":[\"position_s\"]}");

    reg(
        "navigate",
        "Step to the previous or next supported asset in the current file's folder "
        "(wrapping). Requires at least two assets in the folder. Returns the newly-loaded "
        "file plus its folder index and the folder asset count.",
        "{\"type\":\"object\",\"properties\":{\"delta\":{\"type\":\"integer\","
        "\"description\":\"Steps to move: +1 for next, -1 for previous.\"}},"
        "\"required\":[\"delta\"]}");

    reg(
        "get_status",
        "Read the player's live state: whether a video is playing, the current and total "
        "position in seconds, the open file path, the media kind, mute state, the folder "
        "index/count used by navigate, the resolved stereo layout with the signal that "
        "decided it, and the Convert-to-3D state (convert3d_available / _state / _active). "
        "Requires no arguments.",
        "{\"type\":\"object\"}");

    reg(
        "set_layout",
        "Override how the current media is unpacked into left/right eyes, or return to "
        "automatic detection. Mirrors the L key. The override applies to the currently "
        "loaded file only and is dropped when other media is loaded.",
        "{\"type\":\"object\",\"properties\":{\"layout\":{\"type\":\"string\","
        "\"enum\":[\"auto\",\"mono\",\"sbs_full\",\"sbs_half\",\"tb_full\",\"tb_half\"],"
        "\"description\":\"Layout to pin, or 'auto' to restore detection.\"},"
        "\"projection\":{\"type\":\"string\","
        "\"enum\":[\"auto\",\"flat\",\"equirect180\",\"equirect360\",\"fisheye\",\"cubemap\",\"eac\"],"
        "\"description\":\"VR180 / VR360 viewer (#155, mirrors the P key): the spherical projection "
        "each eye holds, or 'flat' for an ordinary picture, or 'auto' (container metadata, then the "
        "file name). cubemap = 360 cubemap in a 3x2 grid (FFmpeg v360 c3x2); eac = 360 equi-angular "
        "cubemap (YouTube EAC, FFmpeg v360 eac). The packing still comes from 'layout'.\"},"
        "\"fisheye_fov_deg\":{\"type\":\"number\",\"minimum\":90,\"maximum\":250,"
        "\"description\":\"Dual-fisheye lens field of view (default 180).\"}}}");

    reg(
        "get_view",
        "VR180 / VR360 viewer (#155): read the view - whether a VR projection is active, the "
        "projection and what decided it, the model (plane = stereo VR180 off-axis window, "
        "cylinder = stereo VR360, free = mono), yaw / pitch / horizontal FOV in degrees, the "
        "life-size default FOV and the clamps. Requires no arguments.",
        "{\"type\":\"object\"}");

    reg(
        "set_view",
        "VR180 / VR360 viewer (#155): steer the window into the scene. yaw_deg (+ = right; for "
        "stereo VR180 an off-axis pan clamped to +-pan_max_deg so the pair stays rectified, for "
        "VR360 a heading that wraps), pitch_deg (+ = up), fov_deg (horizontal; 0 = life-size). "
        "Omitted fields keep their value; recenter=true resets all three first. Errors when no "
        "VR projection is on screen. Returns get_view.",
        "{\"type\":\"object\",\"properties\":{"
        "\"yaw_deg\":{\"type\":\"number\"},\"pitch_deg\":{\"type\":\"number\"},"
        "\"fov_deg\":{\"type\":\"number\",\"minimum\":0},\"recenter\":{\"type\":\"boolean\"}}}");

    reg(
        "list_cameras",
        "List the live cameras the player can open (index + name), in the order open_camera's "
        "numeric selector uses. Tracking/IR cameras (e.g. a 3D panel's eye-tracking camera) "
        "are never openable and are reported separately under 'blocked'. Enumerating wakes "
        "the camera subsystem; it does not open any device.",
        "{\"type\":\"object\"}");

    reg(
        "open_camera",
        "Switch the player to a live camera, replacing the current media. The device opens "
        "asynchronously: poll get_status (camera_state becomes 'streaming'). The layout is "
        "guessed from the frame aspect (16:9 -> SBS-half); set_layout overrides it and "
        "play_pause freezes/resumes the picture.",
        "{\"type\":\"object\",\"properties\":{\"selector\":{\"type\":\"string\","
        "\"description\":\"'auto' (default: SpatialLabs Eyes first), an index from "
        "list_cameras, or a case-insensitive name substring.\"}}}");

    reg(
        "set_auto_convergence",
        "Auto-convergence (#92): measure the subject's stereo disparity and shift the "
        "eyes so it sits at the display plane. Live camera / stream and stereo files (SBS or "
        "TB video and stills; a LIF with its own convergence keeps it). DEFAULT OFF. "
        "'enabled' turns it on/off (the shift ramps back to 0 when off). 'policy' picks the subject: nearest (the nearest "
        "strong depth plane - a person at the camera), sharp (the sharpest blocks - shallow "
        "depth of field), centre (centre-weighted), focus (needs focus_x/focus_y). "
        "focus_x/focus_y (0..1, normalised over one eye's picture) pin the subject and imply "
        "policy focus. Returns the auto_conv_* fields of get_status.",
        "{\"type\":\"object\",\"properties\":{"
        "\"enabled\":{\"type\":\"boolean\"},"
        "\"policy\":{\"type\":\"string\",\"enum\":[\"nearest\",\"sharp\",\"centre\",\"focus\"]},"
        "\"focus_x\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},"
        "\"focus_y\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}");

    reg(
        "open_url",
        "Open a live network stream by URL, replacing the current media: rtsp://, rtmp(s)://, "
        "udp:// or rtp:// (MPEG-TS), http(s):// (progressive TS/FLV/fMP4) or an HLS .m3u8. "
        "Connects asynchronously: poll get_status (stream_state becomes 'streaming'; "
        "'reconnecting' after a loss, 'failed' once it gives up). srt:// is refused. The "
        "layout is guessed from the frame aspect; set_layout overrides it and play_pause "
        "freezes/resumes the picture. Stop it with close_camera, or by opening anything else.",
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\","
        "\"description\":\"Stream URL, e.g. rtsp://192.168.1.10:554/stream1 or "
        "udp://127.0.0.1:5004.\"}},\"required\":[\"url\"]}");

    reg(
        "set_stream_quality",
        "HLS / DASH streams only: pick the rendition (quality). 'auto' (the default for every "
        "new URL) adapts to the measured network throughput; a rendition index (0 = lowest "
        "bandwidth, see get_status stream_renditions) or a height such as '1080p' fixes it. "
        "The change takes effect at the next segment boundary, without a gap in the picture. "
        "Returns the setting and the rendition list.",
        "{\"type\":\"object\",\"properties\":{\"quality\":{\"oneOf\":[{\"type\":\"string\"},"
        "{\"type\":\"integer\",\"minimum\":0}],\"description\":\"'auto', a rendition index, "
        "or a height like '720p'.\"}},\"required\":[\"quality\"]}");

    reg(
        "set_convert_3d",
        "Convert to 3D: have the runtime's 2D->3D conversion turn the current MONO picture "
        "(a 2D still, a mono video or live source, or anything pinned mono with set_layout) "
        "into a stereo pair that replaces it on screen. Mirrors Shift+L and the top-bar "
        "toggle. Off on every load; turning it on asks for the panel's 3D mode if it is flat. "
        "Errors when the runtime offers no conversion, the content is not mono, or the "
        "conversion task cannot serve this session (the error says why). 'provider' (runtimes "
        "with provider choice) picks one of get_status's convert3d_candidates by id for this "
        "session only, or \"\" for the default; it never changes the user's default. "
        "Returns the convert3d_* fields of get_status.",
        "{\"type\":\"object\",\"properties\":{"
        "\"on\":{\"type\":\"boolean\",\"description\":\"true = convert, false = show the "
        "original 2D picture.\"},"
        "\"provider\":{\"type\":\"string\",\"description\":\"A convert3d_candidates id, or "
        "empty for the default.\"}},"
        "\"required\":[\"on\"]}");

    reg(
        "set_input_provider",
        "Ask the runtime for a specific input provider for this app's controllers, hand "
        "tracking or gamepad (XR_DXR_input_providers). 'provider' is an id from get_status's "
        "input_providers list, or \"\" to go back to the default. 'persist' (default false) "
        "also saves it as the user's per-app input setting - the same record DisplayXR's "
        "settings edit; only set it when the user asked to remember the choice. Roles move on "
        "the app's next input sync. Errors when the runtime offers no input-provider choice.",
        "{\"type\":\"object\",\"properties\":{"
        "\"family\":{\"type\":\"string\",\"enum\":[\"controllers\",\"hand_tracking\",\"gamepad\"]},"
        "\"provider\":{\"type\":\"string\",\"description\":\"An input_providers id, or empty for "
        "the default.\"},"
        "\"persist\":{\"type\":\"boolean\"}},"
        "\"required\":[\"family\",\"provider\"]}");

    reg(
        "close_camera",
        "Stop the live camera (or live stream) and return to the idle screen. Errors if "
        "neither is live.",
        "{\"type\":\"object\"}");
}

}  // namespace mp
