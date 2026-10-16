// SPDX-License-Identifier: Apache-2.0
#include "pro/OcioBake.h"

#if defined(MEDIAPLAYER_WITH_OCIO)
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace mp::ocio {

#if defined(MEDIAPLAYER_WITH_OCIO)

bool Available() { return true; }

namespace {
OCIO::ConstConfigRcPtr Load(const std::string& config) {
    if (config.empty()) return OCIO::Config::CreateFromEnv();
    if (config.rfind("ocio://", 0) == 0) return OCIO::Config::CreateFromBuiltinConfig(config.c_str());
    return OCIO::Config::CreateFromFile(config.c_str());
}
}  // namespace

bool Bake(const Request& req, color::CubeLut& out, std::string& err) {
    try {
        OCIO::ConstConfigRcPtr cfg = Load(req.config);
        if (req.src.empty()) {
            err = "an input colour space is needed (--ocio-src)";
            return false;
        }
        OCIO::ConstProcessorRcPtr proc;
        std::string label;
        if (!req.dst.empty()) {
            OCIO::ColorSpaceTransformRcPtr t = OCIO::ColorSpaceTransform::Create();
            t->setSrc(req.src.c_str());
            t->setDst(req.dst.c_str());
            if (!req.looks.empty()) {
                OCIO::LookTransformRcPtr lt = OCIO::LookTransform::Create();
                lt->setSrc(req.src.c_str());
                lt->setDst(req.dst.c_str());
                lt->setLooks(req.looks.c_str());
                proc = cfg->getProcessor(lt);
            } else {
                proc = cfg->getProcessor(t);
            }
            label = req.src + " -> " + req.dst;
        } else {
            const std::string display = req.display.empty() ? cfg->getDefaultDisplay() : req.display;
            const std::string view = req.view.empty() ? cfg->getDefaultView(display.c_str()) : req.view;
            OCIO::DisplayViewTransformRcPtr t = OCIO::DisplayViewTransform::Create();
            t->setSrc(req.src.c_str());
            t->setDisplay(display.c_str());
            t->setView(view.c_str());
            if (!req.looks.empty()) {
                OCIO::LegacyViewingPipelineRcPtr vp = OCIO::LegacyViewingPipeline::Create();
                vp->setDisplayViewTransform(t);
                vp->setLooksOverrideEnabled(true);
                vp->setLooksOverride(req.looks.c_str());
                proc = vp->getProcessor(cfg);
            } else {
                proc = cfg->getProcessor(t);
            }
            label = req.src + " -> " + display + " / " + view;
        }
        OCIO::ConstCPUProcessorRcPtr cpu = proc->getDefaultCPUProcessor();
        const int n = req.size < 2 ? 2 : (req.size > 129 ? 129 : req.size);
        out = color::CubeLut{};
        out.size = n;
        out.title = "OCIO: " + label + (req.looks.empty() ? "" : " (" + req.looks + ")");
        out.rgb.resize((size_t)n * n * n * 3);
        size_t i = 0;
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    out.rgb[i] = (float)r / (n - 1);
                    out.rgb[i + 1] = (float)g / (n - 1);
                    out.rgb[i + 2] = (float)b / (n - 1);
                    i += 3;
                }
        OCIO::PackedImageDesc img(out.rgb.data(), (long)n * n * n, 1, 3);
        cpu->apply(img);
        return true;
    } catch (const OCIO::Exception& e) {
        err = std::string("OpenColorIO: ") + e.what();
        return false;
    }
}

bool Describe(const std::string& config, ConfigInfo& out, std::string& err) {
    try {
        OCIO::ConstConfigRcPtr cfg = Load(config);
        out = ConfigInfo{};
        out.name = cfg->getName() ? cfg->getName() : "";
        for (int i = 0; i < cfg->getNumColorSpaces(); ++i) out.colorSpaces.push_back(cfg->getColorSpaceNameByIndex(i));
        for (int i = 0; i < cfg->getNumDisplays(); ++i) out.displays.push_back(cfg->getDisplay(i));
        out.defaultDisplay = cfg->getDefaultDisplay();
        out.defaultView = cfg->getDefaultView(out.defaultDisplay.c_str());
        for (int i = 0; i < cfg->getNumViews(out.defaultDisplay.c_str()); ++i)
            out.views.push_back(cfg->getView(out.defaultDisplay.c_str(), i));
        return true;
    } catch (const OCIO::Exception& e) {
        err = std::string("OpenColorIO: ") + e.what();
        return false;
    }
}

#else

bool Available() { return false; }
bool Bake(const Request&, color::CubeLut&, std::string& err) {
    err = "this build has no OpenColorIO (configure with MP_OCIO_ROOT); bake a .cube with ociobakelut instead";
    return false;
}
bool Describe(const std::string&, ConfigInfo&, std::string& err) {
    err = "this build has no OpenColorIO";
    return false;
}

#endif

}  // namespace mp::ocio
