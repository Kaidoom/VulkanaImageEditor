#!/usr/bin/env python3
"""Developer-only MobileSAM export. Never run by Vulkana; performs no downloads.

Use an isolated environment with torch, torchvision, timm, numpy and onnx.
The caller supplies a reviewed upstream checkout, checkpoint and CPU runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mobile-sam', type=Path, required=True)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--runtime-notices', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Never replace an existing pack or output folder implicitly.
    args.output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.mobile_sam.resolve()))
    import torch
    from mobile_sam import sam_model_registry
    from mobile_sam.utils.onnx import SamOnnxModel
    torch.set_num_threads(6)
    model = sam_model_registry['vit_t'](checkpoint=str(args.checkpoint)).eval()
    encoder = args.output / 'encoder.onnx'
    decoder = args.output / 'decoder.onnx'
    with torch.no_grad():
        image = torch.zeros(1, 3, 1024, 1024)
        features = model.image_encoder(image)
        torch.onnx.export(model.image_encoder, image, str(encoder), opset_version=17,
                          dynamo=False, input_names=['image'], output_names=['features'])
        wrapper = SamOnnxModel(model, return_single_mask=True).eval()
        inputs = {'image_embeddings': features,
                  'point_coords': torch.tensor([[[64., 64.], [900., 1000.]]]),
                  'point_labels': torch.tensor([[2., 3.]]),
                  'mask_input': torch.zeros(1, 1, 256, 256),
                  'has_mask_input': torch.zeros(1),
                  'orig_im_size': torch.tensor([1024., 1024.])}
        torch.onnx.export(wrapper, tuple(inputs.values()), str(decoder), opset_version=17,
                          dynamo=False, input_names=list(inputs),
                          output_names=['masks', 'scores', 'logits'],
                          dynamic_axes={'point_coords': {1: 'points'}, 'point_labels': {1: 'points'}})
    shutil.copy2(args.runtime, args.output / 'libonnxruntime.so')
    shutil.copy2(args.mobile_sam / 'LICENSE', args.output / 'MobileSAM-LICENSE.txt')
    for source, destination in [('LICENSE', 'ONNXRuntime-LICENSE.txt'),
                                ('ThirdPartyNotices.txt', 'ONNXRuntime-ThirdPartyNotices.txt')]:
        shutil.copy2(args.runtime_notices / source, args.output / destination)
    manifest = {'format': 1, 'model': 'MobileSAM vit_t',
                'checkpointSha256': hashlib.sha256(args.checkpoint.read_bytes()).hexdigest()}
    for name in ['encoder.onnx', 'decoder.onnx', 'libonnxruntime.so']:
        manifest[name] = hashlib.sha256((args.output / name).read_bytes()).hexdigest()
    (args.output / 'vulkana-mobilesam-v1.json').write_text(json.dumps(manifest, indent=2))
    print('Created local model pack:', args.output)


if __name__ == '__main__':
    main()
