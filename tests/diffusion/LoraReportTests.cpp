#include "LoraReport.h"

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
    return 0;
}
