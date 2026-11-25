// SPDX-License-Identifier: Apache-2.0
#include "json.h"

#include "dxrmedia/xmp.h"  // format_number / parse_number

#include <cmath>
#include <cstdint>

namespace dxrmedia {
namespace json {

const Value* Value::get(const std::string& key) const {
	if (type != Type::Object) return nullptr;
	for (const auto& kv : obj)
		if (kv.first == key) return &kv.second;
	return nullptr;
}

Value& Value::set(const std::string& key, Value v) {
	type = Type::Object;
	obj.emplace_back(key, std::move(v));
	return obj.back().second;
}

namespace {

struct Parser {
	const std::string& s;
	size_t i = 0;
	int depth = 0;

	explicit Parser(const std::string& t) : s(t) {}

	void ws() {
		while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
	}
	bool lit(const char* w) {
		size_t n = 0;
		while (w[n]) ++n;
		if (s.compare(i, n, w) != 0) return false;
		i += n;
		return true;
	}
	static void utf8(std::string& o, uint32_t cp) {
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) {
			o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F));
			o += (char)(0x80 | (cp & 0x3F));
		} else {
			o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
			o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
		}
	}
	bool hex4(uint32_t& v) {
		if (i + 4 > s.size()) return false;
		v = 0;
		for (int k = 0; k < 4; ++k) {
			const char c = s[i++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
			else return false;
		}
		return true;
	}
	bool str(std::string& o) {
		if (i >= s.size() || s[i] != '"') return false;
		++i;
		while (i < s.size()) {
			const char c = s[i++];
			if (c == '"') return true;
			if ((unsigned char)c < 0x20) return false;
			if (c != '\\') { o += c; continue; }
			if (i >= s.size()) return false;
			const char e = s[i++];
			switch (e) {
			case '"': o += '"'; break;
			case '\\': o += '\\'; break;
			case '/': o += '/'; break;
			case 'b': o += '\b'; break;
			case 'f': o += '\f'; break;
			case 'n': o += '\n'; break;
			case 'r': o += '\r'; break;
			case 't': o += '\t'; break;
			case 'u': {
				uint32_t cp = 0;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					uint32_t lo = 0;
					if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				utf8(o, cp);
				break;
			}
			default: return false;
			}
		}
		return false;
	}
	bool num(double& v) {
		const size_t st = i;
		if (i < s.size() && s[i] == '-') ++i;
		if (i >= s.size() || !(s[i] >= '0' && s[i] <= '9')) return false;
		while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
		                        s[i] == 'E' || s[i] == '+' || s[i] == '-'))
			++i;
		return parse_number(s.substr(st, i - st), v);
	}
	bool value(Value& v) {
		if (++depth > 64) return false;
		ws();
		if (i >= s.size()) return false;
		bool ok = false;
		const char c = s[i];
		if (c == '{') {
			++i;
			v = Value::object();
			ws();
			if (i < s.size() && s[i] == '}') { ++i; ok = true; }
			else {
				for (;;) {
					ws();
					std::string k;
					if (!str(k)) break;
					ws();
					if (i >= s.size() || s[i] != ':') break;
					++i;
					Value child;
					if (!value(child)) break;
					v.obj.emplace_back(std::move(k), std::move(child));
					ws();
					if (i < s.size() && s[i] == ',') { ++i; continue; }
					if (i < s.size() && s[i] == '}') { ++i; ok = true; }
					break;
				}
			}
		} else if (c == '[') {
			++i;
			v = Value::array();
			ws();
			if (i < s.size() && s[i] == ']') { ++i; ok = true; }
			else {
				for (;;) {
					Value child;
					if (!value(child)) break;
					v.arr.push_back(std::move(child));
					ws();
					if (i < s.size() && s[i] == ',') { ++i; continue; }
					if (i < s.size() && s[i] == ']') { ++i; ok = true; }
					break;
				}
			}
		} else if (c == '"') {
			v = Value::string("");
			ok = str(v.str);
		} else if (lit("true")) { v = Value(); v.type = Value::Type::Bool; v.b = true; ok = true; }
		else if (lit("false")) { v = Value(); v.type = Value::Type::Bool; v.b = false; ok = true; }
		else if (lit("null")) { v = Value(); ok = true; }
		else { v = Value(); v.type = Value::Type::Number; ok = num(v.num); }
		--depth;
		return ok;
	}
};

void writeStr(std::string& o, const std::string& s) {
	o += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				static const char* hx = "0123456789abcdef";
				o += "\\u00";
				o += hx[c >> 4];
				o += hx[c & 15];
			} else {
				o += (char)c;
			}
		}
	}
	o += '"';
}

bool writeVal(std::string& o, const Value& v) {
	switch (v.type) {
	case Value::Type::Null: o += "null"; break;
	case Value::Type::Bool: o += v.b ? "true" : "false"; break;
	case Value::Type::Number:
		if (!std::isfinite(v.num)) return false;
		o += format_number(v.num);
		break;
	case Value::Type::String: writeStr(o, v.str); break;
	case Value::Type::Array:
		o += '[';
		for (size_t k = 0; k < v.arr.size(); ++k) {
			if (k) o += ',';
			if (!writeVal(o, v.arr[k])) return false;
		}
		o += ']';
		break;
	case Value::Type::Object:
		o += '{';
		for (size_t k = 0; k < v.obj.size(); ++k) {
			if (k) o += ',';
			writeStr(o, v.obj[k].first);
			o += ':';
			if (!writeVal(o, v.obj[k].second)) return false;
		}
		o += '}';
		break;
	}
	return true;
}

}  // namespace

bool parse(const std::string& text, Value& out) {
	Parser p(text);
	if (!p.value(out)) return false;
	p.ws();
	return p.i == text.size();
}

bool write(const Value& v, std::string& out) {
	std::string o;
	if (!writeVal(o, v)) return false;
	out = std::move(o);
	return true;
}

}  // namespace json
}  // namespace dxrmedia
