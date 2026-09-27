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
  for (std::size_t type = 0; type < 7; ++type) {
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
  typed.renderCache=u::prepareDocumentSampleCache(typed,1024*1024);typed=c::prepareSpatialFilterLayer(typed);document.insertLayer(2,typed);
  c::TextLayer text;text.utf8="O8";text.defaultStyle.font.family="Noto Sans";text.defaultStyle.sizePixels=22;text.defaultStyle.color={150,210,90,180};
  typed=c::Layer::text("Styled editable text",c::normalizedText(text));typed.effects=all;typed.localToDocument={1,.12,9,-.15,1,25};
  auto filters=std::make_shared<c::SpatialFilterStack>();filters->items[0].enabled=true;filters->items[0].parameters=c::GaussianBlurParameters{1,1};typed.filters=filters;
  typed.renderCache=u::prepareDocumentSampleCache(typed,1024*1024);typed=c::prepareSpatialFilterLayer(typed);document.insertLayer(3,typed);
  scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"typed style stack plus filter",false);
  scene.logicalViewport={76.8,64};scene.viewport.setZoom(1.2);
  verify(document,scene,gpu.render(scene),"styled typed fractional DPI",false);
}
