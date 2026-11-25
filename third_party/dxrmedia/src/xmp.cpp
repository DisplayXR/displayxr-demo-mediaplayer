// SPDX-License-Identifier: Apache-2.0
#include "dxrmedia/xmp.h"

#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <map>
#include <sstream>

namespace dxrmedia {

// ---------------------------------------------------------------- numbers

std::string format_number(double v) {
	if (!std::isfinite(v)) return std::string();
	if (std::fabs(v) < kZeroBelow) return "0";  // FORMAT.md 0: no exponent form, ever
	// Shortest fixed-point text that reads back to exactly v, imbued with the classic
	// locale so the separator is '.' whatever the global C or C++ locale is. For
	// |v| >= 1e-9 a double needs at most 9 + 17 = 26 decimals, so 40 always suffices.
	std::string last;
	for (int p = 0; p <= 40; ++p) {
		std::ostringstream os;
		os.imbue(std::locale::classic());
		os.setf(std::ios::fixed, std::ios::floatfield);
		os.precision(p);
		os << v;
		last = os.str();
		double back = 0;
		if (parse_number(last, back) && back == v) return last;
	}
	return last;
}

bool parse_number(const std::string& s, double& out) {
	if (s.empty() || s.size() > 512) return false;
	for (char c : s)
		if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
			return false;  // rejects "nan", "inf", ',' decimal separators, spaces
	std::istringstream is(s);
	is.imbue(std::locale::classic());
	double v = 0;
	is >> v;
	if (is.fail()) return false;
	if (is.peek() != std::char_traits<char>::eof()) return false;  // trailing junk
	if (!std::isfinite(v)) return false;
	out = v;
	return true;
}

namespace {

bool parse_int(const std::string& s, int64_t& out) {
	if (s.empty() || s.size() > 20) return false;
	size_t i = 0;
	bool neg = false;
	if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
	if (i >= s.size()) return false;
	uint64_t v = 0;
	for (; i < s.size(); ++i) {
		if (s[i] < '0' || s[i] > '9') return false;
		const uint64_t d = (uint64_t)(s[i] - '0');
		if (v > (UINT64_MAX - d) / 10) return false;
		v = v * 10 + d;
	}
	if (!neg && v > (uint64_t)INT64_MAX) return false;
	if (neg && v > (uint64_t)INT64_MAX + 1) return false;
	out = neg ? (int64_t)(0 - v) : (int64_t)v;
	return true;
}

bool parse_int32(const std::string& s, int& out) {
	int64_t v = 0;
	if (!parse_int(s, v) || v < INT32_MIN || v > INT32_MAX) return false;
	out = (int)v;
	return true;
}

// ---------------------------------------------------------------- Calibration JSON

json::Value num(double v) { return json::Value::number(v); }

template <size_t N>
json::Value arr(const std::array<double, N>& a) {
	json::Value v = json::Value::array();
	for (double x : a) v.arr.push_back(num(x));
	return v;
}

json::Value size2(const std::array<int, 2>& a) {
	json::Value v = json::Value::array();
	v.arr.push_back(num(a[0]));
	v.arr.push_back(num(a[1]));
	return v;
}

json::Value lens(const LensIntrinsics& l) {
	json::Value v = json::Value::object();
	v.set("fx", num(l.fx));
	v.set("fy", num(l.fy));
	v.set("cx", num(l.cx));
	v.set("cy", num(l.cy));
	v.set("model", json::Value::string(l.model));
	json::Value c = json::Value::array();
	for (double x : l.coeffs) c.arr.push_back(num(x));
	v.set("coeffs", c);
	return v;
}

void getNum(const json::Value& o, const char* k, double& dst) {
	const json::Value* v = o.get(k);
	if (v && v->isNumber()) dst = v->num;
}

void getStr(const json::Value& o, const char* k, std::string& dst) {
	const json::Value* v = o.get(k);
	if (v && v->isString()) dst = v->str;
}

template <size_t N>
void getArr(const json::Value& o, const char* k, std::array<double, N>& dst) {
	const json::Value* v = o.get(k);
	if (!v || !v->isArray() || v->arr.size() != N) return;
	for (size_t i = 0; i < N; ++i)
		if (!v->arr[i].isNumber()) return;
	for (size_t i = 0; i < N; ++i) dst[i] = v->arr[i].num;
}

void getSize(const json::Value& o, std::array<int, 2>& dst) {
	std::array<double, 2> d{{(double)dst[0], (double)dst[1]}};
	getArr(o, "size", d);
	if (d[0] >= 0 && d[0] <= INT32_MAX && d[1] >= 0 && d[1] <= INT32_MAX) {
		dst[0] = (int)d[0];
		dst[1] = (int)d[1];
	}
}

void getLens(const json::Value& o, const char* k, LensIntrinsics& l) {
	const json::Value* v = o.get(k);
	if (!v || !v->isObject()) return;
	getNum(*v, "fx", l.fx);
	getNum(*v, "fy", l.fy);
	getNum(*v, "cx", l.cx);
	getNum(*v, "cy", l.cy);
	getStr(*v, "model", l.model);
	const json::Value* c = v->get("coeffs");
	if (c && c->isArray()) {
		l.coeffs.clear();
		for (const auto& e : c->arr)
			if (e.isNumber()) l.coeffs.push_back(e.num);
	}
}

}  // namespace

bool calibration_to_json(const Calibration& c, std::string& out, std::string* error) {
	json::Value root = json::Value::object();
	if (c.raw) {
		const CalibrationRaw& r = *c.raw;
		json::Value v = json::Value::object();
		v.set("size", size2(r.size));
		v.set("left", lens(r.left));
		v.set("right", lens(r.right));
		v.set("R_right_from_left", arr(r.rRightFromLeft));
		v.set("T_right_from_left_mm", arr(r.tRightFromLeftMm));
		root.set("raw", v);
	}
	if (c.rectified) {
		const CalibrationRectified& r = *c.rectified;
		json::Value v = json::Value::object();
		v.set("size", size2(r.size));
		json::Value k = json::Value::object();
		k.set("fx", num(r.fx));
		k.set("fy", num(r.fy));
		k.set("cx_left", num(r.cxLeft));
		k.set("cx_right", num(r.cxRight));
		k.set("cy", num(r.cy));
		v.set("K", k);
		v.set("R_left", arr(r.rLeft));
		v.set("R_right", arr(r.rRight));
		if (r.verticalOffset) v.set("vertical_offset", num(*r.verticalOffset));
		if (r.verticalSlope) v.set("vertical_slope", num(*r.verticalSlope));
		root.set("rectified", v);
	}
	if (!c.source.empty()) root.set("source", json::Value::string(c.source));
	if (!c.sourceId.empty()) root.set("source_id", json::Value::string(c.sourceId));
	if (!json::write(root, out)) {
		if (error) *error = "calibration holds a non-finite number";
		return false;
	}
	return true;
}

bool calibration_from_json(const std::string& text, Calibration& out) {
	json::Value root;
	if (!json::parse(text, root) || !root.isObject()) return false;
	out = Calibration();
	if (const json::Value* r = root.get("raw"); r && r->isObject()) {
		CalibrationRaw raw;
		getSize(*r, raw.size);
		getLens(*r, "left", raw.left);
		getLens(*r, "right", raw.right);
		getArr(*r, "R_right_from_left", raw.rRightFromLeft);
		getArr(*r, "T_right_from_left_mm", raw.tRightFromLeftMm);
		out.raw = raw;
	}
	if (const json::Value* r = root.get("rectified"); r && r->isObject()) {
		CalibrationRectified rect;
		getSize(*r, rect.size);
		if (const json::Value* k = r->get("K"); k && k->isObject()) {
			getNum(*k, "fx", rect.fx);
			getNum(*k, "fy", rect.fy);
			getNum(*k, "cx_left", rect.cxLeft);
			getNum(*k, "cx_right", rect.cxRight);
			getNum(*k, "cy", rect.cy);
		}
		getArr(*r, "R_left", rect.rLeft);
		getArr(*r, "R_right", rect.rRight);
		if (const json::Value* vo = r->get("vertical_offset"); vo && vo->isNumber()) rect.verticalOffset = vo->num;
		if (const json::Value* vs = r->get("vertical_slope"); vs && vs->isNumber()) rect.verticalSlope = vs->num;
		out.rectified = rect;
	}
	getStr(root, "source", out.source);
	getStr(root, "source_id", out.sourceId);
	return true;
}

// ---------------------------------------------------------------- XMP

namespace {

// Decode one UTF-8 sequence at s[i]; false (and n = 1) on an invalid one.
bool utf8At(const std::string& s, size_t i, uint32_t& cp, size_t& n) {
	const unsigned char c = (unsigned char)s[i];
	n = 1;
	if (c < 0x80) { cp = c; return true; }
	int len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
	if (!len || i + (size_t)len > s.size()) return false;
	cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
	for (int k = 1; k < len; ++k) {
		const unsigned char d = (unsigned char)s[i + (size_t)k];
		if ((d & 0xC0) != 0x80) return false;
		cp = (cp << 6) | (d & 0x3F);
	}
	if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
	    (cp >= 0xD800 && cp <= 0xDFFF))
		return false;
	n = (size_t)len;
	return true;
}

// Escape for an XML 1.0 attribute value. Characters XML 1.0 cannot carry at all
// (C0 controls except tab/LF/CR, U+FFFE, U+FFFF) are dropped; invalid UTF-8 bytes
// become U+FFFD. The result is always well-formed.
void xmlEscape(std::string& o, const std::string& s) {
	for (size_t i = 0; i < s.size();) {
		uint32_t cp = 0;
		size_t n = 1;
		if (!utf8At(s, i, cp, n)) {
			o += "\xEF\xBF\xBD";
			i += 1;
			continue;
		}
		switch (cp) {
		case '&': o += "&amp;"; break;
		case '<': o += "&lt;"; break;
		case '>': o += "&gt;"; break;
		case '"': o += "&quot;"; break;
		case '\n': o += "&#xA;"; break;
		case '\r': o += "&#xD;"; break;
		case '\t': o += "&#x9;"; break;
		default:
			if (cp < 0x20 || cp == 0xFFFE || cp == 0xFFFF) break;  // forbidden in XML 1.0
			o.append(s, i, n);
		}
		i += n;
	}
}

struct AttrWriter {
	std::string o;
	void str(const char* name, const std::string& v) {
		if (v.empty()) return;
		o += "\n    dxr:";
		o += name;
		o += "=\"";
		xmlEscape(o, v);
		o += '"';
	}
	void dbl(const char* name, const std::optional<double>& v) {
		if (v && std::isfinite(*v)) str(name, format_number(*v));
	}
	void i64(const char* name, const std::optional<int64_t>& v) {
		if (v) str(name, std::to_string(*v));
	}
	void i32(const char* name, const std::optional<int>& v) {
		if (v) str(name, std::to_string(*v));
	}
	void boolean(const char* name, const std::optional<bool>& v) {
		if (v) str(name, *v ? "true" : "false");
	}
};

}  // namespace

bool xmp_serialize(const StereoMeta& m, std::string& p, std::string* error) {
	AttrWriter a;
	a.str("Version", std::to_string(kFormatVersion));
	a.str("Layout", m.layout);
	a.i32("EyeWidth", m.eyeWidth);
	a.i32("EyeHeight", m.eyeHeight);
	a.boolean("Rectified", m.rectified);
	a.str("Alignment", m.alignment);
	a.dbl("Convergence", m.convergence);
	a.dbl("ConvergenceDistanceMm", m.convergenceDistanceMm);
	a.str("ConvergenceSource", m.convergenceSource);
	a.dbl("DisparityNear", m.disparityNear);
	a.dbl("DisparityFar", m.disparityFar);
	a.dbl("BaselineMm", m.baselineMm);
	a.dbl("FocalFrac", m.focalFrac);
	a.dbl("FocalYFrac", m.focalYFrac);
	a.dbl("CxFrac", m.cxFrac);
	a.dbl("CyFrac", m.cyFrac);
	a.str("Facing", m.facing);
	a.dbl("FocusDistanceMm", m.focusDistanceMm);
	a.i64("CaptureTimeLeftNs", m.captureTimeLeftNs);
	a.i64("CaptureTimeRightNs", m.captureTimeRightNs);
	a.str("CalibrationSource", m.calibrationSource);
	a.str("CameraName", m.cameraName);
	a.str("PixelSource", m.pixelSource);
	a.boolean("SourceLossy", m.sourceLossy);
	if (m.calibration) {
		std::string j;
		if (!calibration_to_json(*m.calibration, j, error)) return false;
		a.str("Calibration", j);
	} else {
		a.str("Calibration", m.calibrationJson);
	}
	a.i32("DisparityMapIndex", m.disparityMapIndex);
	a.dbl("DisparityMapNear", m.disparityMapNear);
	a.dbl("DisparityMapFar", m.disparityMapFar);
	a.str("DisparityMapEye", m.disparityMapEye);
	a.i32("RawIndex", m.rawIndex);

	p.clear();
	p += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
	p += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"dxrmedia\">\n";
	p += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
	p += "  <rdf:Description rdf:about=\"\"\n    xmlns:dxr=\"";
	p += kDxrNamespace;
	p += "\"";
	p += a.o;
	p += "/>\n";
	p += " </rdf:RDF>\n";
	p += "</x:xmpmeta>\n";
	p += "<?xpacket end=\"w\"?>";
	return true;
}

namespace {

std::string xmlUnescape(const std::string& s) {
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '&') { o += s[i]; continue; }
		const size_t semi = s.find(';', i);
		if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
		const std::string ent = s.substr(i + 1, semi - i - 1);
		uint32_t cp = 0;
		bool ok = true;
		if (ent == "amp") cp = '&';
		else if (ent == "lt") cp = '<';
		else if (ent == "gt") cp = '>';
		else if (ent == "quot") cp = '"';
		else if (ent == "apos") cp = '\'';
		else if (ent.size() > 1 && ent[0] == '#') {
			const bool hex = ent[1] == 'x' || ent[1] == 'X';
			const std::string digits = ent.substr(hex ? 2 : 1);
			if (digits.empty()) ok = false;
			for (char c : digits) {
				uint32_t d;
				if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
				else if (hex && c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
				else if (hex && c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
				else { ok = false; break; }
				cp = cp * (hex ? 16 : 10) + d;
				if (cp > 0x10FFFF) { ok = false; break; }
			}
		} else ok = false;
		if (!ok) { o += s[i]; continue; }
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) {
			o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F));
			o += (char)(0x80 | (cp & 0x3F));
		} else {
			o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
			o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
		}
		i = semi;
	}
	return o;
}

struct Tag {
	std::string name;  // qualified name
	std::vector<std::pair<std::string, std::string>> attrs;  // raw (escaped) values
	bool selfClosing = false;
	size_t end = 0;  // index just after '>'
};

bool isNameChar(char c) {
	return c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '=' && c != '>' && c != '/' &&
	       c != '<' && c != '"' && c != '\'';
}

// Parse a start tag beginning at s[i] == '<'. False on malformed input.
bool parseStartTag(const std::string& s, size_t i, Tag& t) {
	size_t p = i + 1;
	while (p < s.size() && isNameChar(s[p])) t.name += s[p++];
	if (t.name.empty()) return false;
	for (;;) {
		while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
		if (p >= s.size()) return false;
		if (s[p] == '>') { t.end = p + 1; return true; }
		if (s[p] == '/') {
			if (p + 1 < s.size() && s[p + 1] == '>') { t.selfClosing = true; t.end = p + 2; return true; }
			return false;
		}
		std::string an;
		while (p < s.size() && isNameChar(s[p])) an += s[p++];
		if (an.empty()) return false;
		while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
		if (p >= s.size() || s[p] != '=') return false;
		++p;
		while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
		if (p >= s.size() || (s[p] != '"' && s[p] != '\'')) return false;
		const char q = s[p++];
		const size_t close = s.find(q, p);
		if (close == std::string::npos) return false;
		t.attrs.emplace_back(an, s.substr(p, close - p));
		p = close + 1;
	}
}

std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
	return s.substr(a, b - a);
}

bool ieq(const std::string& a, const char* b) {
	size_t n = std::strlen(b);
	if (a.size() != n) return false;
	for (size_t i = 0; i < n; ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}

void setField(StereoMeta& m, const std::string& k, const std::string& v) {
	double d = 0;
	int i = 0;
	int64_t l = 0;
	auto D = [&](std::optional<double>& dst) { if (parse_number(v, d)) dst = d; };
	auto I = [&](std::optional<int>& dst) { if (parse_int32(v, i)) dst = i; };
	auto L = [&](std::optional<int64_t>& dst) { if (parse_int(v, l)) dst = l; };
	if (k == "Version") I(m.version);
	else if (k == "Layout") m.layout = v;
	else if (k == "EyeWidth") I(m.eyeWidth);
	else if (k == "EyeHeight") I(m.eyeHeight);
	else if (k == "Rectified") {
		if (ieq(v, "true")) m.rectified = true;
		else if (ieq(v, "false")) m.rectified = false;
	} else if (k == "Alignment") m.alignment = v;
	else if (k == "Convergence") D(m.convergence);
	else if (k == "ConvergenceDistanceMm") D(m.convergenceDistanceMm);
	else if (k == "ConvergenceSource") m.convergenceSource = v;
	else if (k == "DisparityNear") D(m.disparityNear);
	else if (k == "DisparityFar") D(m.disparityFar);
	else if (k == "BaselineMm") D(m.baselineMm);
	else if (k == "FocalFrac") D(m.focalFrac);
	else if (k == "FocalYFrac") D(m.focalYFrac);
	else if (k == "CxFrac") D(m.cxFrac);
	else if (k == "CyFrac") D(m.cyFrac);
	else if (k == "Facing") m.facing = v;
	else if (k == "FocusDistanceMm") D(m.focusDistanceMm);
	else if (k == "CaptureTimeLeftNs") L(m.captureTimeLeftNs);
	else if (k == "CaptureTimeRightNs") L(m.captureTimeRightNs);
	else if (k == "CalibrationSource") m.calibrationSource = v;
	else if (k == "CameraName") m.cameraName = v;
	else if (k == "PixelSource") m.pixelSource = v;
	else if (k == "SourceLossy") {
		if (ieq(v, "true")) m.sourceLossy = true;
		else if (ieq(v, "false")) m.sourceLossy = false;
	} else if (k == "Calibration") {
		m.calibrationJson = v;
		Calibration c;
		if (calibration_from_json(v, c)) m.calibration = c;
	} else if (k == "DisparityMapIndex") I(m.disparityMapIndex);
	else if (k == "DisparityMapNear") D(m.disparityMapNear);
	else if (k == "DisparityMapFar") D(m.disparityMapFar);
	else if (k == "DisparityMapEye") m.disparityMapEye = v;
	else if (k == "RawIndex") I(m.rawIndex);
}

}  // namespace

bool xmp_parse(const std::string& s, StereoMeta& out) {
	// Pass 1: every xmlns:prefix declaration (prefixes are document-wide in practice).
	std::map<std::string, std::string> ns;
	std::vector<Tag> tags;
	std::vector<size_t> tagPos;
	for (size_t i = 0; i < s.size();) {
		const size_t lt = s.find('<', i);
		if (lt == std::string::npos) break;
		if (s.compare(lt, 4, "<!--") == 0) {
			const size_t e = s.find("-->", lt + 4);
			if (e == std::string::npos) break;
			i = e + 3;
			continue;
		}
		if (s.compare(lt, 2, "<?") == 0 || s.compare(lt, 2, "<!") == 0) {
			const size_t e = s.find('>', lt + 2);
			if (e == std::string::npos) break;
			i = e + 1;
			continue;
		}
		if (s.compare(lt, 2, "</") == 0) {
			const size_t e = s.find('>', lt + 2);
			if (e == std::string::npos) break;
			i = e + 1;
			continue;
		}
		Tag t;
		if (!parseStartTag(s, lt, t)) { i = lt + 1; continue; }
		for (const auto& a : t.attrs)
			if (a.first.compare(0, 6, "xmlns:") == 0) ns[a.first.substr(6)] = xmlUnescape(a.second);
		i = t.end;
		tagPos.push_back(lt);
		tags.push_back(std::move(t));
	}
	std::map<std::string, bool> isDxr;
	for (const auto& kv : ns)
		if (kv.second == kDxrNamespace) isDxr[kv.first] = true;
	if (isDxr.empty()) return false;

	auto dxrLocal = [&](const std::string& qname, std::string& local) {
		const size_t c = qname.find(':');
		if (c == std::string::npos) return false;
		if (!isDxr.count(qname.substr(0, c))) return false;
		local = qname.substr(c + 1);
		return true;
	};

	StereoMeta m;
	for (const Tag& t : tags) {
		std::string local;
		for (const auto& a : t.attrs)
			if (dxrLocal(a.first, local)) setField(m, local, xmlUnescape(a.second));
		if (!t.selfClosing && dxrLocal(t.name, local)) {
			// Element form: simple text content up to the next tag.
			const size_t lt = s.find('<', t.end);
			if (lt != std::string::npos && s.compare(lt, 2, "</") == 0)
				setField(m, local, trim(xmlUnescape(s.substr(t.end, lt - t.end))));
		}
	}
	out = m;
	return out.version.has_value();
}

}  // namespace dxrmedia
