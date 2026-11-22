#pragma once

#include "backend.hpp"

#include <filesystem>

namespace layerdiff {

// AutoencoderKL supplies the RGB representation. The transparent UNet uses
// that RGB and the same latent to jointly predict straight foreground and alpha.
class Vae {
  public:
    Vae(Backend& backend, const std::filesystem::path& directory);
    Tensor encode(const Tensor& rgb01, const Tensor& posteriorNoise);
    Tensor decode(const Tensor& scaledLatent, const Tensor& alpha01);

  private:
    Backend& backend_;
    Weights rgb_;
    Weights transparency_;
};

} // namespace layerdiff
