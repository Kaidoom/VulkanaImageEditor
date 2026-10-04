void layerEffectRendering(OffscreenCanvas &gpu, Review &review) {
  c::Document document({{72, 56}, 96});
  auto background = c::Layer::raster("Backdrop", patterned({72, 56}, false));
  document.insertLayer(0, background);
  auto layer = c::Layer::raster("Effect source", patterned({28, 24}, true));
  layer.localToDocument = {1.1, .2, 20, -.15, 1.2, 13};
  layer.opacity = .63F;
  layer.blendMode = c::BlendMode::Overlay;
  const auto id = layer.id;
  document.insertLayer(1, layer);
  auto scene = sceneFor(document);
  gpu.render(scene);
  const auto uploads = gpu.stats().fullUploads;
  for (std::size_t type = 0; type < c::layerEffectCount; ++type) {
    auto state = std::make_shared<c::LayerEffectStack>();
    auto &e = state->items[type];
    e.enabled = true;
    e.size = 4.5;
    e.distance = 5.5;
    e.angle = -37;
    e.spread = .35;
    e.color = {120, 60, 210, 210};
    e.secondColor = {255, 170, 30, 20};
    document.setLayerEffects(id, state);
    *document.layer(id) = c::prepareSpatialFilterLayer(*document.layer(id));
    scene.document = document.snapshot();
    const auto image = gpu.render(scene);
    const auto reference =
        verify(document, scene, image,
               c::layerEffectName(c::LayerEffectType(type)), false);
    review.add(type, c::layerEffectName(c::LayerEffectType(type)), image,
               reference);
    const auto before = gpu.stats();
    for (auto mode : c::allBlendModes) {
      auto changed = std::make_shared<c::LayerEffectStack>(*state);
      changed->items[type].blendMode = mode;
      changed->items[type].color = {190, 100, 30, 150};
      document.setLayerEffects(id, changed);
      check(c::layerEffectCacheValid(*document.layer(id)),
            "material change reuses effect geometry");
      scene.document = document.snapshot();
      verify(document, scene, gpu.render(scene), "effect blend mode", false);
    }
    check(gpu.stats().adjustmentMaskUploads == before.adjustmentMaskUploads,
          "effect material edits do not upload masks");
    scene.cursorInside = true;
    scene.cursorLogical = {30, 20};
    const auto idle = gpu.stats();
    gpu.render(scene, false);
    check(gpu.stats().compositionPasses == idle.compositionPasses,
          "effect cursor movement does not recompose");
    document.setLayerCrop(id, c::LayerCrop{1.5, 2.25, 23, 19});
    scene.document = document.snapshot();
    verify(document, scene, gpu.render(scene), "fractional effect crop", false);
    document.setLayerCrop(id, {});
    if(type==7)for(auto style:{c::BevelStyle::Inner,c::BevelStyle::Outer,c::BevelStyle::Emboss}) {
      auto changed=std::make_shared<c::LayerEffectStack>(*state);auto& bevel=changed->items[7];
      bevel.bevel.style=style;bevel.bevel.down=true;bevel.bevel.soften=2;
      bevel.bevel.surface=c::effectContourPreset(false,3);bevel.bevel.gloss=c::effectContourPreset(true,2);
      document.setLayerEffects(id,changed);*document.layer(id)=c::prepareSpatialFilterLayer(*document.layer(id));
      scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"bevel styles and independent contours",false);
    }
    scene.effectBypassLayer = id;
    scene.document = document.snapshot();
    const auto bypass = gpu.render(scene);
    const auto saved = document.layer(id)->effects;
    document.layer(id)->effects.reset();
    verify(document, scene, bypass, "effect-only comparison", false);
    document.layer(id)->effects = saved;
    scene.effectBypassLayer.reset();
  }
  check(gpu.stats().fullUploads == uploads,
        "all effects reuse original source texture");
  auto all = std::make_shared<c::LayerEffectStack>();
  for (auto &e : all->items) {
    e.enabled = true;
    e.size = 3;
    e.opacity = .6;
    e.distance = 5;
  }
  document.setLayerEffects(id, all);
  *document.layer(id) = c::prepareSpatialFilterLayer(*document.layer(id));
  scene.document = document.snapshot();
  verify(document, scene, gpu.render(scene), "all effects fixed order", false);
  c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={29.5,18.25};shape.fillEnabled=false;
  shape.strokeEnabled=true;shape.strokeWidth=2.5;shape.strokeColor={210,100,35,170};
  auto typed=c::Layer::shape("Styled hollow shape",shape);typed.effects=all;typed.localToDocument={-.9,.2,60,.1,.9,9};
  const auto shapeId=typed.id;
  typed.renderCache=u::prepareDocumentSampleCache(typed,1024*1024);typed=c::prepareSpatialFilterLayer(typed);document.insertLayer(2,typed);
  c::TextLayer text;text.utf8="O8";text.defaultStyle.font.family="Noto Sans";text.defaultStyle.sizePixels=22;text.defaultStyle.color={150,210,90,180};
  typed=c::Layer::text("Styled editable text",c::normalizedText(text));typed.effects=all;typed.localToDocument={1,.12,9,-.15,1,25};
  auto filters=std::make_shared<c::SpatialFilterStack>();filters->items[0].enabled=true;filters->items[0].parameters=c::GaussianBlurParameters{1,1};typed.filters=filters;
  typed.renderCache=u::prepareDocumentSampleCache(typed,1024*1024);typed=c::prepareSpatialFilterLayer(typed);document.insertLayer(3,typed);
  scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"typed style stack plus filter",false);
  scene.logicalViewport={76.8,64};scene.viewport.setZoom(1.2);
  verify(document,scene,gpu.render(scene),"styled typed fractional DPI",false);
  const auto textId=typed.id;
  auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({28,24},128);mask->outside=0;
  document.setLayerMask(id,mask);
  const auto relief=document.layer(id)->effectCache;
  auto tree=document.tree();const auto group=c::makeLayerId();
  tree.roots={background.id,group};tree.containers.push_back({group,"Clipped relief",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{id,shapeId,textId}});
  check(document.replaceStructure(document.tree(),tree),"bevel clipping hierarchy");
  auto correction=c::Layer::adjustment("After relief");auto adjustments=std::make_shared<c::AdjustmentStack>();
  adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{.4};correction.adjustments=adjustments;
  document.insertLayer(document.layers().size(),correction);
  scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"bevel clipping plus downstream adjustment and soft mask",false);
  check(document.layer(id)->effectCache==relief,"output mask and downstream adjustment reuse bevel geometry");
  auto transform=document.layer(id)->localToDocument;transform.m20=.002;transform.m21=-.001;
  document.setLayerTransform(id,transform);scene.document=document.snapshot();
  verify(document,scene,gpu.render(scene),"projective relief with clipping and operator",false);
}
void bevelBenchmarks(bool validation) {
  for(const auto extent:{VkExtent2D{3840,2160},VkExtent2D{5120,2880}}) {
    c::Document doc({{extent.width,extent.height},96});
    auto bg=c::Layer::raster("Backdrop",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{extent.width,extent.height},c::Rgba8{40,65,90,255}));
    doc.insertLayer(0,bg);
    auto layer=c::Layer::raster("Large relief",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{extent.width-120,extent.height-120},c::Rgba8{170,90,30,210}));
    layer.localToDocument.m02=60;layer.localToDocument.m12=60;
    auto state=std::make_shared<c::LayerEffectStack>();auto& bevel=state->items[7];
    bevel.enabled=true;bevel.size=16;bevel.bevel.style=c::BevelStyle::Emboss;layer.effects=state;
    layer=c::prepareSpatialFilterLayer(layer);const auto cache=layer.effectCache;
    const auto id=layer.id;doc.insertLayer(1,layer);
    auto scene=sceneFor(doc);scene.logicalViewport={double(extent.width),double(extent.height)};
    OffscreenCanvas gpu(validation,extent);gpu.render(scene,false);const auto cold=gpu.frameMilliseconds();const auto initial=gpu.stats();
    double material=0,lighting=0;
    for(int n=0;n<6;++n) {
      state=std::make_shared<c::LayerEffectStack>(*state);state->items[7].opacity=.3+n*.08;
      doc.setLayerEffects(id,state);scene.document=doc.snapshot();gpu.render(scene,false);material+=gpu.frameMilliseconds();
    }
    check(gpu.stats().adjustmentMaskBytes==initial.adjustmentMaskBytes,"large bevel material scrub zero mask upload");
    const auto before=gpu.stats();
    for(int n=0;n<3;++n) {
      state=std::make_shared<c::LayerEffectStack>(*state);state->items[7].angle=-100+n*45;
      doc.setLayerEffects(id,state);*doc.layer(id)=c::prepareSpatialFilterLayer(*doc.layer(id));
      check(doc.layer(id)->effectCache->bevelNormals==cache->bevelNormals,"large bevel relight reuses normals");
      scene.document=doc.snapshot();gpu.render(scene,false);lighting+=gpu.frameMilliseconds();
    }
    check(gpu.stats().uploadedBytes==initial.uploadedBytes,"large bevel parameters never upload source again");
    const auto idle=gpu.stats();gpu.render(scene,false);check(gpu.stats().compositionPasses==idle.compositionPasses,"large bevel unchanged frame no recomposition");
    std::cout<<"Bevel GPU "<<extent.width<<'x'<<extent.height<<" validation="<<validation<<" cold_frame_ms="<<cold
      <<" material_frame_ms="<<material/6<<" relight_frame_ms="<<lighting/3
      <<" relight_mask_bytes="<<gpu.stats().adjustmentMaskBytes-before.adjustmentMaskBytes
      <<" source_reuploads="<<gpu.stats().fullUploads-initial.fullUploads<<" mask_buffer_capacity="<<gpu.stats().adjustmentBufferCapacity<<'\n';
  }
}
