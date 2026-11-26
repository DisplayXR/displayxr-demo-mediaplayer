// SPDX-License-Identifier: Apache-2.0
//
// Minimal JSON value, parser and compact writer — enough for the Calibration JSON
// (FORMAT.md 1.5). Internal to dxrmedia.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dxrmedia {
namespace json {

struct Value {
	enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
	bool b = false;
	double num = 0;
	std::string str;
	std::vector<Value> arr;
	// Insertion order kept so the writer's output is stable.
	std::vector<std::pair<std::string, Value>> obj;

	static Value number(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
	static Value string(std::string s) { Value x; x.type = Type::String; x.str = std::move(s); return x; }
	static Value array() { Value x; x.type = Type::Array; return x; }
	static Value object() { Value x; x.type = Type::Object; return x; }

	bool isNumber() const { return type == Type::Number; }
	bool isString() const { return type == Type::String; }
	bool isArray() const { return type == Type::Array; }
	bool isObject() const { return type == Type::Object; }

	const Value* get(const std::string& key) const;   // object member or nullptr
	Value& set(const std::string& key, Value v);       // append (no dedupe)
};

// Depth-limited (64) recursive descent; rejects trailing garbage.
bool parse(const std::string& text, Value& out);
// False when the value holds a non-finite number (JSON has no NaN/Inf).
bool write(const Value& v, std::string& out);

}  // namespace json
}  // namespace dxrmedia
