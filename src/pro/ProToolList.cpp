// SPDX-License-Identifier: Apache-2.0
//
// Media Player Pro's agent tools as a table (see ProToolList.h); ProTools.cpp dispatches
// them. Descriptions are agent-facing API docs.
#include "pro/ProToolList.h"

namespace mp::pro {

void RegisterProAgentTools(const AgentToolRegisterFn& reg) {
    reg("set_grade",
        "Set Media Player Pro's grade: a .cube 3D LUT path, an ASC CDL file (.cc/.ccc/.cdl), "
        "exposure in stops (-4..4, linear light) and bypass. HDR sources (HLG, PQ, scene-linear "
        "EXR): tone_map auto (roll off HLG / PQ, clip EXR at white) | on | off (clip) | raw (the "
        "signal unmapped, for a show LUT that expects PQ / HLG), peak_nits = the source's "
        "brightest (0 = from the stream, else 1000; for HLG the display peak it is rendered for), "
        "untagged = how a source with no HDR tag is taken: sdr (default) | hlg | pq. Arguments left out keep their "
        "current value; with no arguments it only reads the grade. Returns the grade: LUT (with "
        "lut_loading / lut_error), CDL (slope/offset/power/saturation), exposure, bypass, the display "
        "calibration and the HDR mapping (tone_map, peak_nits, hdr = what the current source gets: "
        "transfer, raw, roll_off, bt2020, peak_nits). A LUT loads in the background: the reply carries "
        "lut_loading; call set_grade with no arguments to see the LUT (or lut_error) once it applies.",
        "{\"type\":\"object\",\"properties\":{\"lut\":{\"type\":\"string\"},\"cdl\":{\"type\":\"string\"},"
        "\"exposure\":{\"type\":\"number\"},\"bypass\":{\"type\":\"boolean\"},"
        "\"tone_map\":{\"type\":\"string\",\"enum\":[\"auto\",\"on\",\"off\",\"raw\"]},"
        "\"peak_nits\":{\"type\":\"number\"},"
        "\"untagged\":{\"type\":\"string\",\"enum\":[\"sdr\",\"hlg\",\"pq\"]}}}");
    reg("set_target_screen",
        "Judge the depth for the screen the picture is made for (#135): target cinema, tv, monitor, "
        "tablet or off, or a custom width_m + distance_m. With a target, get_rig_status reports near / "
        "far in its terms (mm, arcminutes, background divergence) and the rig check alerts when the "
        "background diverges past 0.5 deg there. preview (default true) shows the picture on the panel "
        "at the angular size it has on the target, so the depth reads as it will there (the picture "
        "crops or gets a border). Returns the target and the depth in its terms, for the picture as shown "
        "(convergence included: parallax_offset_pct). picture_scale in the reply is the last frame's; "
        "get_rig_status has the new one a frame later.",
        "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"enum\":[\"off\",\"cinema\",\"tv\",\"monitor\",\"tablet\"]},"
        "\"width_m\":{\"type\":\"number\",\"minimum\":0.1},\"distance_m\":{\"type\":\"number\",\"minimum\":0.2},"
        "\"preview\":{\"type\":\"boolean\"}}}");
    reg("set_display_calibration",
        "Set this panel's display calibration (#133): a .cube 3D LUT (domain 0..1) from a calibration "
        "tool (DisplayCAL, Calman, ColourSpace), applied after the grade and QC views. It is kept for "
        "this panel and loads automatically from then on (keep: false = this run only). clear: true "
        "removes it (and the kept file); bypass turns it off without removing it. The show grade's "
        "bypass (set_grade bypass, G) does not affect it. Returns the panel key and the calibration; "
        "the LUT loads in the background, so the reply may carry loading.",
        "{\"type\":\"object\",\"properties\":{\"cube\":{\"type\":\"string\"},\"keep\":{\"type\":\"boolean\"},"
        "\"clear\":{\"type\":\"boolean\"},\"bypass\":{\"type\":\"boolean\"}}}");
    reg("set_qc_view",
        "Set the QC view (not set_view, which steers the VR180 / VR360 window): "
        "3d, left, right, difference (|left - right| x4), anaglyph or false_colour; "
        "optional zoom (1 = fit) applied to both eyes; channel isolation (rgb, red, green, blue, "
        "alpha, luma); display gamma (0.25..4, 1 = none); a wipe (off, grade = source | graded, "
        "eyes = left eye | right eye, clips = this clip | the set_compare B clip) at wipe_pos (0..1 across "
        "the picture). Arguments left out keep their value; with no arguments it only reads the "
        "view. Returns the QC view.",
        "{\"type\":\"object\",\"properties\":{\"view\":{\"type\":\"string\",\"enum\":[\"3d\",\"left\",\"right\","
        "\"difference\",\"anaglyph\",\"false_colour\"]},\"zoom\":{\"type\":\"number\",\"minimum\":1},"
        "\"channel\":{\"type\":\"string\",\"enum\":[\"rgb\",\"red\",\"green\",\"blue\",\"alpha\",\"luma\"]},"
        "\"gamma\":{\"type\":\"number\",\"minimum\":0.25,\"maximum\":4},"
        "\"wipe\":{\"type\":\"string\",\"enum\":[\"off\",\"grade\",\"eyes\",\"clips\"]},"
        "\"wipe_pos\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1}}}");
    reg("get_rig_status",
        "Read the live rig checks for a side-by-side stereo source: vertical error (full-res px, + = "
        "right eye lower), roll (degrees), near / far screen parallax (% of eye width, negative = in "
        "front of the screen), edge violations (edge_left_pct / edge_right_pct: the most in-front "
        "parallax along each frame edge, null = not measured), colour match between the eyes (CIE76 dE), eye sync (frames between "
        "the paired inputs, -1 = unknown), the depth budget, recent alerts with timecode, and the "
        "source timecode and capture-to-screen delay.",
        "{\"type\":\"object\"}");
    reg("set_depth_budget",
        "Set the limits the rig checks alert on: near / far parallax in % of eye width (near "
        "negative = in front), vertical error in px, colour match in dE, and edge_pct: the nearest "
        "parallax allowed at the left / right frame edge (an edge violation below it).",
        "{\"type\":\"object\",\"properties\":{\"near_pct\":{\"type\":\"number\"},\"far_pct\":{\"type\":\"number\"},"
        "\"vertical_px\":{\"type\":\"number\"},\"delta_e\":{\"type\":\"number\"},"
        "\"edge_pct\":{\"type\":\"number\"}}}");
    reg("set_depth_overlay", "Show or hide the depth overlay (the rig check's matches on the picture).",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":[\"on\"]}");
    reg("snapshot",
        "Capture both eyes as shown (graded) to a PNG in Pictures/DisplayXR, with a JSON note "
        "beside it: the note text, timecode and rig numbers.",
        "{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\"}}}");
    reg("export_qc_report",
        "Write this session's QC report to a new Pictures/DisplayXR/qc_<time> folder: report.html "
        "(alerts, snapshots with both eyes, notes, rig numbers, depth budget; self-contained), "
        "markers.edl (DaVinci Resolve timeline markers) and markers.csv. Returns the paths at once "
        "(writing: true); the files are written in the background, report.html last, so the "
        "report is complete once report.html exists.",
        "{\"type\":\"object\"}");
    reg("list_inputs", "List the station's configured live inputs (name, spec) and which one is live.",
        "{\"type\":\"object\"}");
    reg("select_input", "Switch to a configured input, by index (from list_inputs) or name.",
        "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"name\":{\"type\":\"string\"}}}");
    reg("set_ocio",
        "Bake an OpenColorIO transform into the grade's LUT: config (a path, an ocio:// built-in, or "
        "empty for $OCIO), src = the picture's colour space, then display + view (default: the "
        "config's) or dst = a target colour space; optional looks. Needs a build with OpenColorIO. "
        "Bakes in the background: poll set_grade (no arguments) until lut_loading is empty (lut or "
        "lut_error).",
        "{\"type\":\"object\",\"properties\":{\"config\":{\"type\":\"string\"},\"src\":{\"type\":\"string\"},"
        "\"display\":{\"type\":\"string\"},\"view\":{\"type\":\"string\"},\"dst\":{\"type\":\"string\"},"
        "\"looks\":{\"type\":\"string\"}},\"required\":[\"src\"]}");
    reg("describe_ocio", "List an OpenColorIO config's colour spaces, displays and the default display's views.",
        "{\"type\":\"object\",\"properties\":{\"config\":{\"type\":\"string\"}}}");
    reg("set_resolve_follow",
        "The live Resolve panel link. on = make the station follow (or stop following) Resolve's "
        "playhead; with no arguments it only reads the link. Returns whether the panel is running, "
        "Resolve's timeline, playhead timecode and the clip under it, whether the station follows "
        "it, and whether Resolve is playing (inferred from the playhead).",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}}}");
    reg("set_compare",
        "A/B compare: set the B clip (a file path); toggle=true (or no path) swaps to the other clip "
        "at the current position.",
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"toggle\":{\"type\":\"boolean\"}}}");
    reg("open_decklink",
        "Open a Blackmagic DeckLink SDI/HDMI input as the live source. device = index or part of "
        "the name; right = the right eye's device (two inputs paired by timecode); stereo = "
        "auto|mono|sbs|dual|pair; input = sdi|hdmi; mode = starting format FourCC (e.g. Hp25).",
        "{\"type\":\"object\",\"properties\":{\"device\":{\"type\":\"string\"},\"right\":{\"type\":\"string\"},"
        "\"stereo\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}}}");
}

}  // namespace mp::pro
