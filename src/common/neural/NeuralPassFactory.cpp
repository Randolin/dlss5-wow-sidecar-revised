#include "neural/NeuralPassFactory.h"

#include "neural/DirectNrPass.h"
#include "neural/PassthroughPass.h"

namespace sidecar {

NrPassSetup SetupFromSettings(const NrSettings& nr) {
  NrPassSetup setup;
  for (const auto& s : nr.Effective()) {
    NrPassTuning t;
    t.preset = static_cast<uint32_t>(s.preset);
    t.style = static_cast<uint32_t>(s.style);
    t.intensity = s.intensity;
    t.localStructure = s.localStructure;
    t.localTone = s.localTone;
    t.skinStructure = s.skinStructure;
    t.autoMask = s.autoMask;
    t.uiCorrection = s.uiCorrection;
    setup.passes.push_back(t);
  }
  setup.modelScale = nr.modelScale;
  setup.chainComposed = nr.chainComposed;
  setup.finalFull = nr.finalPassFull;
  return setup;
}

NrBridgeParams BridgeFromSettings(const NrSettings& nr) {
  NrBridgeParams b;
  b.enabled = nr.bridge;
  b.paperWhiteNits = nr.paperWhiteNits;
  b.strength = 1.0f;   // "blend" is retired; the model's intensity is the dial
  b.colourPreserve = nr.colourPreserve;
  b.highlightProtect = nr.highlightProtect;
  b.split = nr.splitView;
  return b;
}

std::unique_ptr<INeuralPass> MakeNeuralPass(std::string_view name,
                                            const NeuralPassContext& context,
                                            std::vector<std::string>& warnings) {
  if (name == "passthrough") return PassthroughPass::Create();

  if (name == "reshade" || name == "ngx") {
    // Retired routes. "ngx" was the name for the direct path when it was
    // believed unreachable; "reshade" was the add-on-hosted route that the
    // direct path replaced. Both mean "direct" now.
    warnings.emplace_back("neural_pass \"" + std::string(name) +
                          "\" is retired; using \"direct\"");
    return MakeNeuralPass("direct", context, warnings);
  }

  if (name == "direct") {
    if (!context.device) {
      warnings.emplace_back("neural_pass \"direct\" needs a graphics device; "
                            "using passthrough");
      return PassthroughPass::Create();
    }

    DirectNrPass::Options options;
    options.runtimeDir = context.runtimeDir;
    options.width = context.width;
    options.height = context.height;
    options.arch = context.arch;
    options.syntheticDepth = context.syntheticDepth;
    options.depthGradient = context.depthGradient;
    options.depthInverted = context.depthInverted;
    options.setup = SetupFromSettings(context.nr);
    options.bridge = BridgeFromSettings(context.nr);
    options.timestampFrequency = context.timestampFrequency;

    std::string reason;
    if (auto pass = DirectNrPass::Create(context.device, options, reason)) {
      return pass;
    }
    warnings.emplace_back("neural_pass \"direct\" is unavailable (" + reason +
                          "); using passthrough");
    return PassthroughPass::Create();
  }

  warnings.emplace_back("neural_pass \"" + std::string(name) +
                        "\" is not a known pass; using passthrough");
  return PassthroughPass::Create();
}

std::unique_ptr<INeuralPass> MakeNeuralPass(std::string_view name,
                                            std::vector<std::string>& warnings) {
  return MakeNeuralPass(name, NeuralPassContext{}, warnings);
}

}  // namespace sidecar
