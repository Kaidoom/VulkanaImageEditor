#include "LoraReport.h"
#include "name_conversion.h"

int main()
{
    vulkana::diffusion::LoraReport report;
    report.observe("(0 / 12) LoRA tensors have been applied, lora_file_path = /models/a.safetensors --- lora.hpp:1045\n");
    if (report.compatible("/models/a.safetensors")) return 1;
    report.observe("(12 / 12) LoRA tensors have been applied, lora_file_path = /models/a.safetensors --- lora.hpp:1045\n");
    report.observe("(0 / 0) LoRA tensors have been applied, lora_file_path = /models/a.safetensors --- lora.hpp:1045\n");
    if (!report.compatible("/models/a.safetensors")) return 2;
    report.observe("(4 / 4) LoRA tensors have been applied, lora_file_path = /models/b.safetensors --- lora.hpp:1045\n");
    report.observe("(2 / 6) incompatible LoRA tensors have been skipped, lora_file_path = /models/b.safetensors --- lora.hpp:1049\n");
    if (report.compatible("/models/b.safetensors")) return 3;
    if (!report.compatible("/models/a.safetensors")) return 4;
    if (report.compatible("/models/failed-to-load.safetensors")) return 5;
    report.files.clear();
    if (report.compatible("/models/a.safetensors")) return 6;
    const std::string refiner = "token_refiner.blocks.0.attn.out_proj";
    const std::string target = "lora.model.diffusion_model." + refiner + ".weight.";
    if (convert_tensor_name("lora." + refiner + ".lora_A.weight", VERSION_MINIMAX_H3_IMAGE) != target + "lora_down") return 7;
    if (convert_tensor_name("lora." + refiner + ".lora_B.weight", VERSION_MINIMAX_H3_IMAGE) != target + "lora_up") return 8;
    if (convert_tensor_name("lora.transformer." + refiner + ".alpha", VERSION_MINIMAX_H3_IMAGE) != target + "alpha") return 9;
    return 0;
}
