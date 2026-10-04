void blendV3Consumers(OffscreenCanvas& gpu)
{
    c::Document doc({{72,56},96});
    auto base=c::Layer::raster("Partial-alpha base",patterned({72,56},true));
    const auto baseId=base.id;doc.insertLayer(0,base);
    auto top=c::Layer::raster("Seeded coverage",patterned({32,28},true));
    const auto id=top.id;top.blendSeed=0xdeadbeefU;top.opacity=.57F;top.blendMode=c::BlendMode::Dissolve;
    top.localToDocument={1,0,13,0,1,11};doc.insertLayer(1,top);
    auto scene=sceneFor(doc);
    auto image=gpu.render(scene);verify(doc,scene,image,"Dissolve native local cells",false);
    const auto uploads=gpu.stats().uploadedBytes;
    check(gpu.render(scene)==image,"Dissolve repeat frame identity");
    for(auto transform:std::array{c::AffineTransform{1,0,13.25,0,1,11.75},c::AffineTransform{-.9,.2,55,.1,1.1,8},
        *c::rectangleToQuad({0,0,32,28},{{{3,9},{65,4},{57,46},{14,38}}})}) {
        doc.setLayerTransform(id,transform);scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),"Dissolve transformed local grid",false);
    }
    doc.setLayerTransform(id,top.localToDocument);
    scene=sceneFor(doc);
    auto fx=std::make_shared<c::LayerEffectStack>();
    for(auto i:{0,1,3,5,7}) {fx->items[std::size_t(i)].enabled=true;fx->items[std::size_t(i)].size=3;
        fx->items[std::size_t(i)].blendMode=c::BlendMode::Dissolve;fx->items[std::size_t(i)].opacity=.5;}
    fx->items[7].bevel.shadowBlend=c::BlendMode::Dissolve;
    doc.setLayerEffects(id,fx);*doc.layer(id)=c::prepareSpatialFilterLayer(*doc.layer(id));
    scene.document=doc.snapshot();verify(doc,scene,gpu.render(scene),"Dissolve styles including bevel and whole contribution",false);
    auto adj=c::Layer::adjustment("Correction");const auto aid=adj.id;adj.opacity=.43F;adj.blendSeed=777;
    auto stack=std::make_shared<c::AdjustmentStack>();stack->items[9].enabled=true;adj.adjustments=stack;
    doc.insertLayer(2,adj);
    for(auto mode:c::allBlendModes) {
        doc.setLayerBlendMode(aid,mode);scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),std::string("adjustment ")+std::string(c::blendModeName(mode)),false);
    }
    check(gpu.stats().uploadedBytes==uploads,"Dissolve/adjustment mode edits retain original source textures");
    (void)baseId;
    c::Document cells({{96,80},96});
    auto paper=c::Layer::raster("Blue",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{96,80},c::Rgba8{0,0,255,255}));
    auto ink=c::Layer::raster("Red",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{96,80},c::Rgba8{255,0,0,128}));
    ink.rasterOrigin={-48,-40};ink.localToDocument={1,0,48,0,1,40};ink.blendMode=c::BlendMode::Dissolve;
    cells.insertLayer(0,paper);cells.insertLayer(1,ink);
    for(auto seed:{0U,0xffffffffU,0x7fc00000U,0x12345678U}) {
        cells.layer(ink.id)->blendSeed=seed;
        QImage previous;
        for(float opacity:{0.F,.25F,.5F,.75F,1.F}) {
            cells.setLayerOpacity(ink.id,opacity);auto frame=sceneFor(cells);const auto rendered=gpu.render(frame);
            bool exact=true,monotonic=true;
            for(int y=2;y<78;++y)for(int x=2;x<94;++x) {
                const bool expected=c::blend_detail::bDissolve((128.F/255)*opacity,float(x)-47.5F,float(y)-39.5F,seed);
                const bool accepted=rendered.pixelColor(x,y).red()==255;
                exact&=expected==accepted;
                if(!previous.isNull()&&previous.pixelColor(x,y).red()==255)monotonic&=accepted;
            }
            check(exact,"CPU/GLSL exact gates, negative cells and all seed bits");
            check(monotonic,"GPU opacity occupancy is monotonic");previous=rendered;
        }
    }
    // Opaque backdrop makes this a direct occupancy/color check independent
    // of the sRGB framebuffer's fixed-function alpha-blend precision.
    auto scaled=sceneFor(cells);
    for(double dpr:{1.,1.25,2.})for(double zoom:{.5,1.,2.}) {
        scaled.logicalViewport={96/dpr,80/dpr};scaled.devicePixelRatio=dpr;
        scaled.viewport.setZoom(zoom);scaled.viewport.setPan({.25,-.75});
        verify(cells,scaled,gpu.render(scaled),"Dissolve fixed local field across zoom/DPI",false);
    }
}
