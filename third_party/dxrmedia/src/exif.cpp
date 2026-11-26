// SPDX-License-Identifier: Apache-2.0
#include "exif.h"

#include "tiff.h"

#include <cmath>

namespace dxrmedia {

namespace {

enum : uint16_t {
	kMake = 0x010F,
	kModel = 0x0110,
	kOrientation = 0x0112,
	kXResolution = 0x011A,
	kYResolution = 0x011B,
	kResolutionUnit = 0x0128,
	kSoftware = 0x0131,
	kYCbCrPositioning = 0x0213,
	kExifIfd = 0x8769,
	kExposureTime = 0x829A,
	kFNumber = 0x829D,
	kIso = 0x8827,
	kExifVersion = 0x9000,
	kComponentsConfiguration = 0x9101,
	kDateTimeOriginal = 0x9003,
	kOffsetTimeOriginal = 0x9011,
	kSubjectDistance = 0x9206,
	kFocalLength = 0x920A,
	kUserComment = 0x9286,
	kFlashpixVersion = 0xA000,
	kColorSpace = 0xA001,
	kPixelX = 0xA002,
	kPixelY = 0xA003,
};

const uint8_t kAsciiCharset[8] = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};

void addRational(tiff::Builder& b, uint16_t tag, const std::optional<double>& v) {
	if (!v || !std::isfinite(*v) || *v < 0) return;
	uint32_t n = 0, d = 1;
	tiff::to_rational(*v, n, d);
	b.rational(tag, n, d);
}

}  // namespace

std::vector<uint8_t> exif_build(const ExifMeta& m, const std::string& facing) {
	const bool be = true;
	tiff::Builder ifd0;
	ifd0.be = be;
	if (!m.make.empty()) ifd0.ascii(kMake, m.make);
	if (!m.model.empty()) ifd0.ascii(kModel, m.model);
	ifd0.u16(kOrientation, 1);  // FORMAT.md 0: pixels are upright
	ifd0.rational(kXResolution, 72, 1);
	ifd0.rational(kYResolution, 72, 1);
	ifd0.u16(kResolutionUnit, 2);
	if (!m.software.empty()) ifd0.ascii(kSoftware, m.software);
	ifd0.u16(kYCbCrPositioning, 1);
	ifd0.u32(kExifIfd, 0);  // patched below

	tiff::Builder ex;
	ex.be = be;
	addRational(ex, kExposureTime, m.exposureTimeS);
	addRational(ex, kFNumber, m.fNumber);
	if (m.iso) ex.u16(kIso, (uint16_t)(*m.iso > 65535 ? 65535 : *m.iso));
	ex.undefined(kExifVersion, {'0', '2', '3', '2'});
	ex.undefined(kComponentsConfiguration, {1, 2, 3, 0});  // Y Cb Cr
	if (!m.dateTimeOriginal.empty()) ex.ascii(kDateTimeOriginal, m.dateTimeOriginal);
	if (!m.offsetTimeOriginal.empty()) ex.ascii(kOffsetTimeOriginal, m.offsetTimeOriginal);
	addRational(ex, kSubjectDistance, m.subjectDistanceM);
	addRational(ex, kFocalLength, m.focalLengthMm);
	std::string comment = m.userComment;
	if (comment.empty() && !facing.empty()) comment = "DisplayXR stereo v1; " + facing;
	if (!comment.empty()) {
		std::vector<uint8_t> uc(kAsciiCharset, kAsciiCharset + 8);
		uc.insert(uc.end(), comment.begin(), comment.end());
		ex.undefined(kUserComment, uc);
	}
	ex.undefined(kFlashpixVersion, {'0', '1', '0', '0'});
	ex.u16(kColorSpace, 1);
	if (m.pixelXDimension) ex.u32(kPixelX, *m.pixelXDimension);
	if (m.pixelYDimension) ex.u32(kPixelY, *m.pixelYDimension);

	std::vector<uint8_t> out = {'M', 'M', 0, 42, 0, 0, 0, 8};
	size_t patchAt = 0;
	ifd0.write(out, 8, 0, kExifIfd, &patchAt);
	const uint32_t exOff = (uint32_t)out.size();
	tiff::set32(out, patchAt, exOff, be);
	ex.write(out, exOff, 0);
	return out;
}

bool exif_parse(const uint8_t* p, size_t n, ExifMeta& out) {
	tiff::Reader r;
	r.b = p;
	r.n = n;
	uint32_t first = 0;
	if (!r.header(first)) return false;
	std::vector<tiff::Reader::Field> f0;
	uint32_t next = 0;
	if (!r.ifd(first, f0, next)) return false;
	ExifMeta m;
	using R = tiff::Reader;
	uint32_t u = 0;
	double d = 0;
	if (const auto* f = R::find(f0, kMake)) m.make = r.asString(*f);
	if (const auto* f = R::find(f0, kModel)) m.model = r.asString(*f);
	if (const auto* f = R::find(f0, kSoftware)) m.software = r.asString(*f);
	if (const auto* f = R::find(f0, kOrientation); f && r.asU32(*f, u)) m.orientation = (uint16_t)u;
	const auto* exPtr = R::find(f0, kExifIfd);
	if (exPtr && r.asU32(*exPtr, u)) {
		std::vector<tiff::Reader::Field> fe;
		if (r.ifd(u, fe, next)) {
			if (const auto* f = R::find(fe, kExposureTime); f && r.asRational(*f, d)) m.exposureTimeS = d;
			if (const auto* f = R::find(fe, kFNumber); f && r.asRational(*f, d)) m.fNumber = d;
			if (const auto* f = R::find(fe, kIso); f && r.asU32(*f, u)) m.iso = u;
			if (const auto* f = R::find(fe, kDateTimeOriginal)) m.dateTimeOriginal = r.asString(*f);
			if (const auto* f = R::find(fe, kOffsetTimeOriginal)) m.offsetTimeOriginal = r.asString(*f);
			if (const auto* f = R::find(fe, kSubjectDistance); f && r.asRational(*f, d)) m.subjectDistanceM = d;
			if (const auto* f = R::find(fe, kFocalLength); f && r.asRational(*f, d)) m.focalLengthMm = d;
			if (const auto* f = R::find(fe, kUserComment); f && f->valueSize >= 8) {
				std::vector<uint8_t> b = r.asBytes(*f);
				std::string s(b.begin() + 8, b.end());
				while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
				m.userComment = s;
			}
			if (const auto* f = R::find(fe, kPixelX); f && r.asU32(*f, u)) m.pixelXDimension = u;
			if (const auto* f = R::find(fe, kPixelY); f && r.asU32(*f, u)) m.pixelYDimension = u;
		}
	}
	out = m;
	return true;
}

}  // namespace dxrmedia
