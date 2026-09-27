// Runs the real compositing shader against the CPU output-space sampler. The
// independent double-precision clipping oracle lives in LayerCropCoreTests.
void layerCropRendering(OffscreenCanvas& gpu)
{
    c::Document doc({{96,80},96});
    auto backdrop=c::Layer::raster("Crop backdrop",patterned({96,80},false));
    auto raster=c::Layer::raster("Retained RGBA",patterned({73,61},true));
    raster.localToDocument={.91,-.24,20.123,.17,1.04,3.876};
    raster.opacity=.73f;raster.blendMode=c::BlendMode::SoftLight;
    raster.adjustments=adjustedFixture(c::AdjustmentType::Curves);
    u::QtShapeRenderService shapes;
    c::ShapeLayer geometry;geometry.kind=c::ShapeKind::RoundedRectangle;
    geometry.size={39.5,32.75};geometry.fillColor={211,67,23,139};
    geometry.strokeEnabled=true;geometry.strokeWidth=3.125;geometry.strokeColor={24,219,132,183};
    auto shape=c::Layer::shape("Editable stroked crop",geometry);
    shape.localToDocument={-1.1,.2,75.125,.1,.9,36.125};
    shape.opacity=.81f;shape.blendMode=c::BlendMode::Multiply;
    shape.renderCache=shapes.render({geometry,1.5});
    shape.adjustments=adjustedFixture(c::AdjustmentType::Exposure);
    c::TextLayer text;text.utf8="O ffi\nCrop";
    text.defaultStyle.font={"Noto Sans","Regular",400,false};
    text.defaultStyle.sizePixels=18;text.defaultStyle.color={244,211,97,181};
    text=c::normalizedText(text);
    auto type=c::Layer::text("Editable text crop",text);
    type.localToDocument={.87,.17,5.125,-.15,1.06,5.875};
    type.opacity=.68f;type.blendMode=c::BlendMode::Screen;
    u::QtTextLayoutService fonts;type.renderCache=fonts.layout({text,1.5}).cache;
    const std::array ids {raster.id,shape.id,type.id};
    for(const auto* l:{&backdrop,&raster,&shape,&type})check(doc.insertLayer(doc.layers().size(),*l),"insert crop source");
    auto scene=sceneFor(doc);const auto original=gpu.render(scene);
    verify(doc,scene,original,"uncropped raster + adjusted shape + text",false);
    const auto baseline=gpu.stats();
    const auto rasterRevision=std::get<c::RasterLayer>(raster.payload).surface->revision();
    const auto typedPixels=shape.renderCache->surface;
    for(std::size_t stage=0;stage<5;++stage){
        c::LayerCrop rect {c::RectD{8.25+double(stage)*2.125,4.5-double(stage),31.5-double(stage)*2,27.25}};
        rect.corners={double(stage)*1.25,double(stage)*2.5,double(stage)*3.25,double(stage)*.75};
        for(auto id:ids)check(doc.setLayerCrop(id,rect),"crop metadata update");
        scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),"transformed crop after adjustments before blends",false);
    }
    const auto cropped=gpu.render(scene);
    check(cropped!=original,"cropping changes appearance");
    const auto composed=gpu.stats();
    gpu.render(scene);
    check(gpu.stats().compositionPasses==composed.compositionPasses,"idle crop reuses composition");
    scene.cropPreviewLayer=raster.id;
    check(gpu.render(scene)!=cropped,"retained source preview is visible only when requested");
    scene.cropPreviewLayer.reset();
    check(gpu.render(scene)==cropped,"hiding retained source returns exact cropped appearance");
    for(double zoom:{.35,.7,1.25,2.7}){
        scene.logicalViewport={76.8,64};scene.viewport.setZoom(zoom);
        verify(doc,scene,gpu.render(scene),"fractional-DPR crop coverage",false);
    }
    // Empty crop is not 'no crop', and never erases retained content.
    for(auto id:ids)check(doc.setLayerCrop(id,c::RectD{12,13,0,0}),"empty crop accepted");
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"active empty crops",false);
    for(auto id:ids)check(doc.setLayerCrop(id,{}),"remove crop");
    scene=sceneFor(doc);check(gpu.render(scene)==original,"uncrop restores byte-exact canvas");
    check(gpu.stats().uploadedBytes==baseline.uploadedBytes&&gpu.stats().fullUploads==baseline.fullUploads,
        "crop, preview, zoom and uncrop never upload original images");
    check(doc.layer(shape.id)->renderCache->surface==typedPixels
        &&std::get<c::RasterLayer>(doc.layer(raster.id)->payload).surface->revision()==rasterRevision,
        "crop does not regenerate typed caches or change raster revision");
    // Containers retain pass-through crop behavior and do not crop twice.
    const auto previous=doc.tree();auto tree=previous;
    const auto folder=c::makeLayerId();tree.roots={backdrop.id,folder};
    tree.containers.push_back({folder,"Cropped children",c::ContainerKind::Folder,c::ColorLabel::None,
        {raster.id,shape.id,type.id},true});
    check(doc.replaceStructure(previous,tree),"folder crop fixture");
    check(doc.setLayerCrop(raster.id,c::RectD{3.25,4.5,42,37}),"crop nested layer");
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"pass-through cropped typed hierarchy",false);

    // Exercise the shipping border shader as well as compositing: all eight
    // logical hit targets must receive a visible handle, in either mode.
    OffscreenCanvas preview(true,{640,480});
    c::Document visual({{600,440},96});
    auto photo=c::Layer::raster("Chamfer overlay",patterned({600,440},false));
    c::LayerCrop frame{40,40,520,360};frame.corners={80,120,150,30};
    photo.crop=frame;check(visual.insertLayer(0,photo),"insert chamfer overlay source");
    auto view=sceneFor(visual);view.logicalViewport={640,480};
    view.overlayAccent={246,151,40,255};
    const auto bare=preview.render(view);const auto textureStats=preview.stats();
    r::TransformOverlay overlay{};overlay.localToDocument={1,0,frame.x,0,1,frame.y};
    overlay.geometryExtent=c::Extent2d{frame.width,frame.height};overlay.rotationEnabled=false;
    overlay.cropGeometry=frame;overlay.cropGeometry->x=overlay.cropGeometry->y=0;
    for(bool mode:{false,true}){
        overlay.chamferHandles=mode;view.transformOverlay=overlay;
        const auto painted=preview.render(view);
        for(const auto h:overlay.handles()){
            const auto p=view.viewport.documentToViewport(h,{600,440},view.logicalViewport);
            check(painted.pixelColor(int(p.x),int(p.y))!=bare.pixelColor(int(p.x),int(p.y)),
                "Vulkan handle agrees with logical chamfer hit target");
        }
        if(mode && !qEnvironmentVariable("IMAGEEDITOR_CROP_REVIEW").isEmpty())
            check(painted.save(qEnvironmentVariable("IMAGEEDITOR_CROP_REVIEW")),"save chamfer review image");
    }
    check(preview.stats().uploadedBytes==textureStats.uploadedBytes,"chamfer overlay reuses source textures");
}
