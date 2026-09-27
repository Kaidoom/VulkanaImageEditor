// Reuses the shipping-renderer offscreen host and comparison helpers above.
c::AdjustmentState adjustedFixture(c::AdjustmentType type)
{
    auto state=std::make_shared<c::AdjustmentStack>();
    auto& item=state->items[std::size_t(type)]; item.enabled=true;
    switch(type) {
    case c::AdjustmentType::Exposure: std::get<c::ExposureParameters>(item.parameters).stops=1.25; break;
    case c::AdjustmentType::BrightnessContrast: item.parameters=c::BrightnessContrastParameters {.12,.27}; break;
    case c::AdjustmentType::Levels: {
        auto& p=std::get<c::LevelsParameters>(item.parameters);
        p.channels[0]={.07,1.4,.92,.03,.96}; p.channels[2]={0,.8,1,0,1}; break;
    }
    case c::AdjustmentType::Curves: {
        auto& p=std::get<c::CurvesParameters>(item.parameters);
        p.channels[0].points={{0,.07},{.2,.32},{.53,.43},{.78,.91},{1,.96}};
        p.channels[1].points={{0,0},{.3,.65},{.65,.35},{1,1}}; break;
    }
    case c::AdjustmentType::HueSaturation: {
        auto& p=std::get<c::HueSaturationParameters>(item.parameters);
        p.ranges[0]={-38,.3,.05}; p.ranges[1]={22,-.1,.2}; p.ranges[6]={-40,.2,-.15}; break;
    }
    case c::AdjustmentType::Vibrance: std::get<c::VibranceParameters>(item.parameters).amount=.85; break;
    case c::AdjustmentType::ColorBalance: {
        auto& p=std::get<c::ColorBalanceParameters>(item.parameters);
        p.tones={{{.35,-.25,.1},{-.2,.35,.3},{.2,.1,-.3}}}; break;
    }
    case c::AdjustmentType::WarmthTint: item.parameters=c::WarmthTintParameters {.8,-.35}; break;
    case c::AdjustmentType::BlackWhite: {
        auto& p=std::get<c::BlackWhiteParameters>(item.parameters);
        p.contributions={.22,.1,-.2,.15,-.2,.1}; p.tintStrength=.35; break;
    }
    case c::AdjustmentType::Invert: break;
    }
    return state;
}

void capturedRegionRendering(OffscreenCanvas& gpu)
{
    c::Document document({{96,80},96});
    auto layer=c::Layer::raster("Static captured overlay",patterned({96,80},false));
    check(document.insertLayer(0,layer),"insert captured region fixture");
    auto scene=sceneFor(document);
    const auto original=gpu.render(scene);
    const auto before=gpu.stats();
    // A captured outer region and its hole have independent boundaries.
    scene.capturedRegionEdges=std::make_shared<const std::vector<c::SelectionEdge>>(
        std::vector<c::SelectionEdge>{{{12,10},{70,10}},{{70,10},{70,64}},{{70,64},{12,64}},{{12,64},{12,10}},
            {{25,25},{40,25}},{{40,25},{40,40}},{{40,40},{25,40}},{{25,40},{25,25}}});
    scene.capturedRegionRevision=1;
    const auto outlined=gpu.render(scene);
    check(outlined!=original,"captured region draws visible Vulkan geometry");
    check(outlined.pixel(20,20)==original.pixel(20,20),"no opaque fill inside captured region");
    check(outlined.pixel(32,32)==original.pixel(32,32),"hole artwork remains unobscured");
    const auto cached=gpu.stats();
    check(cached.compositionPasses==before.compositionPasses,"show region does not recompose artwork");
    check(cached.selectionGeometryUploads>before.selectionGeometryUploads,"region uses cached edge stream");
    check(cached.uploadedBytes==before.uploadedBytes&&cached.adjustmentMaskUploads==before.adjustmentMaskUploads,
        "show region uploads no raster pixels or adjustment masks");
    scene.selectionPhase=137.5;
    check(gpu.render(scene)==outlined,"captured outline does not animate with marching ants");
    check(gpu.stats().selectionGeometryUploads==cached.selectionGeometryUploads,"unchanged captured edges reuse GPU buffer");
    scene.capturedRegionEdges.reset();
    check(gpu.render(scene)==original,"hide region restores exact original canvas");
    check(gpu.stats().compositionPasses==before.compositionPasses,"hide region only redraws overlays");
    // Fractional logical size and off-canvas endpoints: shader clips only the
    // reminder, leaving the existing document/workspace placement untouched.
    scene.logicalViewport={76.8,64};
    scene.viewport.setZoom(.6);
    const auto uncluttered=gpu.render(scene);
    scene.capturedRegionEdges=std::make_shared<const std::vector<c::SelectionEdge>>(
        std::vector<c::SelectionEdge>{{{-20,8},{120,8}},{{15,-30},{15,110}}});
    scene.capturedRegionRevision=2;
    const auto clipped=gpu.render(scene);
    check(clipped!=uncluttered,"fractional display scale draws captured overlay");
    const r::CanvasCoordinateMapping mapping({96,80},scene.logicalViewport,{96,80},scene.viewport);
    const auto rect=mapping.documentCanvasFramebufferRect();
    for(int y=0;y<80;++y)for(int x=0;x<96;++x){
        if(x+.5<rect[0]||y+.5<rect[1]||x+.5>=rect[0]+rect[2]||y+.5>=rect[1]+rect[3])
            check(clipped.pixel(x,y)==uncluttered.pixel(x,y),"captured region never bleeds into workspace");
    }
}

void adjustmentRendering(OffscreenCanvas& gpu,Review& review)
{
    c::Document doc({{96,80},96});
    auto base=c::Layer::raster("Backdrop",patterned({96,80},false));
    auto layer=c::Layer::raster("Adjusted source",patterned({80,70},true));
    // Avoid putting document sample centers exactly on a transformed half-open
    // source edge: CPU double/GPU float geometry can classify that tie differently.
    // Color/alpha comparisons still cover the complete interior and AA pixels.
    layer.localToDocument={.99,-.16,13.123,.13,.91,2.876}; layer.opacity=.77f;
    layer.blendMode=c::BlendMode::SoftLight;
    const auto id=layer.id; const auto pixels=std::get<c::RasterLayer>(layer.payload).surface;
    const auto originalRevision=pixels->revision();
    check(doc.insertLayer(0,base) && doc.insertLayer(1,layer),"insert adjustment fixtures");
    auto scene=sceneFor(doc); const auto original=gpu.render(scene);
    check(gpu.stats().adjustmentMaskUploads==0,"unadjusted document needs no dummy mask upload");
    verify(doc,scene,original,"unadjusted adjustment-test fixture");
    auto combined=std::make_shared<c::AdjustmentStack>();
    for (std::size_t i=0;i<c::adjustmentCount;++i) {
        const auto state=adjustedFixture(c::allAdjustmentTypes[i]);
        combined->items[i]=state->items[i];
        doc.setLayerAdjustments(id,state); scene.document=doc.snapshot();
        const auto before=gpu.stats();
        const auto actual=gpu.render(scene);
        const auto expected=verify(doc,scene,actual,c::adjustmentName(c::allAdjustmentTypes[i]));
        review.add(i,c::adjustmentName(c::allAdjustmentTypes[i]),actual,expected);
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"adjustment parameter change never uploads source pixels");
        scene.adjustmentBypassLayer=id;
        check(gpu.render(scene)==original,"comparison bypasses only target adjustments before blending");
        scene.adjustmentBypassLayer.reset();
        check(gpu.render(scene)==actual,"comparison release restores adjusted image");
        const auto cached=gpu.stats();
        scene.cursorInside=true; scene.activeTool=c::ToolId::Brush; scene.cursorLogical={20,20};
        gpu.render(scene);
        check(gpu.stats().compositionPasses==cached.compositionPasses,"adjusted content reused for overlay-only frame");
        check(gpu.stats().adjustmentParameterUploads==cached.adjustmentParameterUploads,"overlay doesn't upload adjustment parameters");
        scene.cursorInside=false; scene.activeTool=c::ToolId::Move;
    }
    doc.setLayerAdjustments(id,combined); scene.document=doc.snapshot();
    const auto both=gpu.render(scene);
    review.add(c::adjustmentCount,"All enabled",both,verify(doc,scene,both,"complete adjustment stack"));
    check(pixels->revision()==originalRevision,"all adjustments preserve authoritative source revision");
    // Captured selection has holes, partial coverage, disconnected regions and
    // follows the layer after flips/rotation rather than following later masks.
    std::vector<std::uint8_t> coverage(96*80);
    for(int y=5;y<72;++y)for(int x=3;x<89;++x)
        if((x<35 || x>52) && (x+y)%7) coverage[std::size_t(y*96+x)]=std::uint8_t((x%3)==0?127:255);
    const auto selection=c::SelectionMask::fromR8({96,80},coverage,96);
    for (auto type:c::allAdjustmentTypes) {
        auto state=std::make_shared<c::AdjustmentStack>(*adjustedFixture(type));
        state->items[std::size_t(type)].mask=c::captureAdjustmentMask(selection,layer.localToDocument);
        doc.setLayerAdjustments(id,state); scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),std::string(c::adjustmentName(type))+" captured partial mask");
        const auto stats=gpu.stats();
        doc.setSelection(c::SelectionMask::filled({96,80},0));
        doc.setLayerTransform(id,{-.87,.21,82.75,.19,1.04,-1.25}); scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),std::string(c::adjustmentName(type))+" transformed local mask");
        check(gpu.stats().adjustmentMaskUploads==stats.adjustmentMaskUploads,"layer transforms reuse immutable captured mask atlas");
        check(gpu.stats().uploadedBytes==stats.uploadedBytes,"masked transforms reuse source textures");
        doc.setLayerTransform(id,layer.localToDocument);
    }
    auto empty=std::make_shared<c::AdjustmentStack>(*adjustedFixture(c::AdjustmentType::Invert));
    empty->items[9].mask=c::captureAdjustmentMask(c::SelectionMask::filled({96,80},0),layer.localToDocument);
    doc.setLayerAdjustments(id,empty); scene.document=doc.snapshot();
    check(gpu.render(scene)==original,"active empty captured mask never becomes unrestricted");
    // Fixed independent expected exposure: encoded byte 64 decoded, doubled in
    // linear light, then re-encoded, with opaque source and no backdrop.
    c::Document exposure({{96,80},96});
    auto solid=c::Layer::raster("Independent exposure",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u {96,80},c::Rgba8 {64,64,64,255}));
    auto ev=std::make_shared<c::AdjustmentStack>(); ev->items[0].enabled=true;
    std::get<c::ExposureParameters>(ev->items[0].parameters).stops=1; solid.adjustments=ev;
    exposure.insertLayer(0,std::move(solid));
    const auto image=gpu.render(sceneFor(exposure));
    const double linear=std::pow((64./255.+.055)/1.055,2.4)*2;
    const int expected=int(std::lround((1.055*std::pow(linear,1./2.4)-.055)*255));
    const auto pixel=image.pixelColor(48,40);
    check(std::abs(pixel.red()-expected)<=1 && pixel.alpha()==255,"independent exposure stop GPU reference");
}

void adjustmentTypedLayers(OffscreenCanvas& gpu)
{
    c::Document doc({{96,80},96});
    auto base=c::Layer::raster("Base",patterned({96,80},false));
    auto shape=c::Layer::shape("Editable shape",c::ShapeLayer {});
    auto shapeData=std::get<c::ShapeLayer>(shape.payload);
    shapeData.size={42,27};
    shapeData.fillColor={240,100,25,155};
    shapeData.strokeColor={25,100,240,210}; shapeData.strokeWidth=3; shapeData.strokeEnabled=true;
    shape.payload=shapeData; shape.localToDocument={.9,.35,30,-.15,1.1,8};
    u::QtShapeRenderService shapeService;
    shape.renderCache=shapeService.render({shapeData,1});
    c::TextLayer text; text.utf8="AaO"; text.defaultStyle.sizePixels=24;
    text.defaultStyle.color={90,210,80,165};
    auto label=c::Layer::text("Editable text",text); label.localToDocument={1,.15,6,.08,1,40};
    text.defaultStyle.font={"Noto Sans","Regular",400,false}; text=c::normalizedText(text);
    label.payload=text;
    u::QtTextLayoutService textService; label.renderCache=textService.layout({text,1}).cache;
    const auto shapeId=shape.id,textId=label.id;
    doc.insertLayer(0,std::move(base)); doc.insertLayer(1,shape); doc.insertLayer(2,label);
    auto scene=sceneFor(doc); gpu.render(scene); const auto before=gpu.stats();
    for (auto type:c::allAdjustmentTypes) {
        doc.setLayerAdjustments(shapeId,adjustedFixture(type)); doc.setLayerAdjustments(textId,adjustedFixture(type));
        scene.document=doc.snapshot(); verify(doc,scene,gpu.render(scene),std::string(c::adjustmentName(type))+" text/shape");
    }
    check(gpu.stats().uploadedBytes==before.uploadedBytes,"typed adjustment changes don't rebuild source caches/textures");
    check(doc.layer(shapeId)->renderCache==shape.renderCache && doc.layer(textId)->renderCache==label.renderCache,
        "editable source cache identity preserved");
}

void adjustmentRamps(OffscreenCanvas& gpu,Review& review)
{
    // Human-readable reference bands supplement the hostile randomized alpha
    // edges above: gray, saturated hue, muted hue, RGB ramps, alpha ramp.
    c::Document doc({{96,80},96});
    std::vector<std::byte> bytes(96*80*4);
    for(int y=0;y<80;++y)for(int x=0;x<96;++x) {
        const double t=double(x)/95;
        QColor color;
        if(y<16)color=QColor::fromRgbF(t,t,t);
        else if(y<32)color=QColor::fromHsvF(double(x)/96,1,.85);
        else if(y<48)color=QColor::fromHsvF(double(x)/96,.25,.7);
        else if(y<64)color=QColor::fromRgbF(t,1-t,.35);
        else color=QColor(210,80,25,int(std::lround(t*255)));
        const std::array channels{color.red(),color.green(),color.blue(),color.alpha()};
        for(std::size_t i=0;i<4;++i)bytes[std::size_t(y*96+x)*4+i]=std::byte(channels[i]);
    }
    auto layer=c::Layer::raster("Reference ramps",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{96,80},std::move(bytes)));const auto id=layer.id;doc.insertLayer(0,std::move(layer));
    auto scene=sceneFor(doc);auto actual=gpu.render(scene);
    review.add(0,"Original",actual,verify(doc,scene,actual,"unadjusted ramps"));
    auto combined=std::make_shared<c::AdjustmentStack>();
    for(std::size_t i=0;i<c::adjustmentCount;++i) {
        auto state=adjustedFixture(c::allAdjustmentTypes[i]);combined->items[i]=state->items[i];
        doc.setLayerAdjustments(id,state);scene.document=doc.snapshot();actual=gpu.render(scene);
        review.add(i+1,c::adjustmentName(c::allAdjustmentTypes[i]),actual,verify(doc,scene,actual,"adjusted reference ramps"));
    }
    doc.setLayerAdjustments(id,combined);scene.document=doc.snapshot();actual=gpu.render(scene);
    review.add(11,"All enabled",actual,verify(doc,scene,actual,"combined reference ramps"));
    // Exercise optional shader branches and exact endpoints as well as each
    // effect's representative setting; no CPU-only fast path can hide these.
    for(int variant=0;variant<8;++variant) {
        auto state=std::make_shared<c::AdjustmentStack>();
        if(variant<2){state->items[0].enabled=true;std::get<c::ExposureParameters>(state->items[0].parameters).stops=variant?-20:20;}
        else if(variant<4){state->items[2].enabled=true;auto& p=std::get<c::LevelsParameters>(state->items[2].parameters);
            p.channels[0]={.5,variant==2?.1:10,.5,1,0};}
        else if(variant==4){state->items[4].enabled=true;auto& p=std::get<c::HueSaturationParameters>(state->items[4].parameters);
            p.colorize=true;p.colorizeHue=320;p.colorizeSaturation=.8;}
        else if(variant==5){*state=*adjustedFixture(c::AdjustmentType::ColorBalance);
            std::get<c::ColorBalanceParameters>(state->items[6].parameters).preserveLuminosity=false;}
        else if(variant==6){state->items[8].enabled=true;auto& p=std::get<c::BlackWhiteParameters>(state->items[8].parameters);
            p.tintColor={30,200,170,0};p.tintStrength=1;}
        else {state->items[3].enabled=true;std::get<c::CurvesParameters>(state->items[3].parameters).channels[0].points={{.1,.95},{.5,.2},{.9,.8}};}
        doc.setLayerAdjustments(id,state);scene.document=doc.snapshot();
        verify(doc,scene,gpu.render(scene),"optional branches and endpoint ramps");
    }
}

void adjustmentResourceLimits(OffscreenCanvas& gpu)
{
    c::Document doc({{96,80},96});
    auto layer=c::Layer::raster("Budgeted source",patterned({96,80},true));
    const auto id=layer.id;const auto source=std::get<c::RasterLayer>(layer.payload).surface;
    const auto revision=source->revision();doc.insertLayer(0,std::move(layer));
    auto scene=sceneFor(doc);const auto original=gpu.render(scene);
    auto disabled=std::make_shared<c::AdjustmentStack>();
    disabled->items[0].mask=c::captureAdjustmentMask(c::SelectionMask::filled({4096,4096},173),{});
    auto before=gpu.stats();doc.setLayerAdjustments(id,disabled);scene.document=doc.snapshot();gpu.render(scene);
    check(gpu.stats().adjustmentMaskBuilds==before.adjustmentMaskBuilds
        &&gpu.stats().adjustmentMaskUploads==before.adjustmentMaskUploads,"never-used disabled mask remains unloaded");
    auto enabled=std::make_shared<c::AdjustmentStack>(*disabled);enabled->items[0].enabled=true;
    std::get<c::ExposureParameters>(enabled->items[0].parameters).stops=1;
    doc.setLayerAdjustments(id,enabled);doc.setLayerVisibility(id,false);scene.document=doc.snapshot();gpu.render(scene);
    check(gpu.stats().adjustmentMaskBuilds==before.adjustmentMaskBuilds,"never-visible masks remain unloaded");
    doc.setLayerVisibility(id,true);scene.document=doc.snapshot();gpu.render(scene);before=gpu.stats();
    doc.setLayerAdjustments(id,disabled);scene.document=doc.snapshot();gpu.render(scene);
    doc.setLayerAdjustments(id,enabled);scene.document=doc.snapshot();gpu.render(scene);
    check(gpu.stats().adjustmentMaskBuilds==before.adjustmentMaskBuilds
        &&gpu.stats().adjustmentMaskUploads==before.adjustmentMaskUploads,"disable/re-enable reuses resident mask");
    // Nine independent, fully varying 4K R8 planes exceed 128MiB. They are
    // legal document data; exceeding a derived cache budget must be recoverable.
    std::vector<std::uint8_t> maskBytes(4096*4096);
    for(std::size_t i=0;i<maskBytes.size();++i)maskBytes[i]=std::uint8_t(i%251+1);
    auto excessive=std::make_shared<c::AdjustmentStack>();
    for(std::size_t i=0;i<9;++i){
        excessive->items[i]=adjustedFixture(c::allAdjustmentTypes[i])->items[i];
        maskBytes[0]=std::uint8_t(i+1);
        excessive->items[i].mask=c::captureAdjustmentMask(c::SelectionMask::fromR8({4096,4096},maskBytes,4096),{});
    }
    doc.setLayerAdjustments(id,excessive);scene.document=doc.snapshot();gpu.render(scene);
    check(!gpu.stats().compositionError.empty(),"over-budget adjustments report recoverable canvas error");
    check(doc.layer(id)->adjustments==excessive&&source->revision()==revision,"render budget failure preserves all document content");
    before=gpu.stats();gpu.render(scene);
    check(gpu.stats().adjustmentMaskBuilds==before.adjustmentMaskBuilds,"failed mask set is not rescanned on overlay frames");
    doc.setLayerAdjustments(id,{});scene.document=doc.snapshot();
    check(gpu.render(scene)==original&&gpu.stats().compositionError.empty(),"disabling offending adjustments restores rendering without restart");
}

void adjustmentBenchmarks(bool validation)
{
    for (auto extent:{VkExtent2D {3840,2160},VkExtent2D {5120,2880}}) {
        c::Document doc({{extent.width,extent.height},96});
        auto stack=std::make_shared<c::AdjustmentStack>();
        for (std::size_t i=0;i<c::adjustmentCount;++i) stack->items[i]=adjustedFixture(c::allAdjustmentTypes[i])->items[i];
        const auto mask=c::SelectionMask::rectangle({extent.width,extent.height},
            {int(extent.width/4),int(extent.height/4),int(extent.width/2),int(extent.height/2)},191);
        stack->items[3].mask=c::captureAdjustmentMask(mask,{});
        c::LayerId topId=0;
        for (int i=0;i<3;++i) {
            auto layer=c::Layer::raster("4K/5K adjustment source",std::make_shared<c::ContiguousRasterSurface>(
                c::Extent2u {extent.width,extent.height},c::Rgba8 {std::uint8_t(40+i*60),std::uint8_t(210-i*45),std::uint8_t(65+i*70),std::uint8_t(i?173:255)}));
            layer.adjustments=stack; layer.opacity=i?.8f:1.f;
            layer.blendMode=i?c::BlendMode::SoftLight:c::BlendMode::Normal; topId=layer.id;
            check(doc.insertLayer(std::size_t(i),std::move(layer)),"benchmark fixture insertion");
        }
        auto scene=sceneFor(doc); scene.logicalViewport={double(extent.width),double(extent.height)};
        OffscreenCanvas gpu(validation,extent);
        gpu.render(scene,false); const double cold=gpu.frameMilliseconds();
        std::vector<double> timings;
        for (int frame=0;frame<20;++frame) {
            auto state=std::make_shared<c::AdjustmentStack>(*stack);
            std::get<c::ExposureParameters>(state->items[0].parameters).stops=-.75+.1*frame;
            doc.setLayerAdjustments(topId,state); scene.document=doc.snapshot();
            const auto before=gpu.stats(); gpu.render(scene,false);
            check(gpu.stats().uploadedBytes==before.uploadedBytes,"4K/5K scrubbing never uploads original textures");
            check(gpu.stats().adjustmentMaskUploads==before.adjustmentMaskUploads,"scrubbing reuses captured mask buffers");
            check(gpu.stats().adjustmentBufferAllocations==before.adjustmentBufferAllocations,"warm adjustment GPU allocations are stable");
            if(frame>=4)timings.push_back(gpu.frameMilliseconds());
        }
        const auto before=gpu.stats(); scene.cursorInside=true; scene.activeTool=c::ToolId::Brush;
        for (int i=0;i<4;++i) {scene.cursorLogical={200.0+i*15,200};gpu.render(scene,false);}
        check(gpu.stats().compositionPasses==before.compositionPasses,"idle adjusted canvas stays demand-driven");
        double sum=0;for(auto t:timings)sum+=t;
        std::sort(timings.begin(),timings.end());
        std::cout << "Adjustment benchmark " << extent.width << 'x' << extent.height
            << ", three full-size layers, ten effects each, partial captured mask: cold=" << cold
            << " ms, scrub mean=" << sum/double(timings.size()) << " ms, max=" << timings.back()
            << " ms, param+mask capacity=" << double(before.adjustmentBufferCapacity)/(1024*1024)
            << " MiB, source bytes=" << before.uploadedBytes << ", mask uploads=" << before.adjustmentMaskUploads << '\n';
    }
}
