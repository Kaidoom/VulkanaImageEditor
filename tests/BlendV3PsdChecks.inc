void blendV3Psd(QString dir)
{
    constexpr std::array modes{c::BlendMode::Dissolve,c::BlendMode::DarkerColor,c::BlendMode::LighterColor,
        c::BlendMode::VividLight,c::BlendMode::LinearLight,c::BlendMode::PinLight,c::BlendMode::HardMix};
    const std::array<QByteArray,7> keys{"diss","dkCl","lgCl","vLit","lLit","pLit","hMix"};
    const std::array<QByteArray,7> descriptors{"Dslv","darkerColor","lighterColor","vividLight","linearLight","pinLight","hardMix"};
    QImage sheet(560,544,QImage::Format_RGBA8888);sheet.fill(QColor("#202329"));
    QPainter painter(&sheet);painter.setPen(QColor("#eeeeee"));
    for(std::size_t i=0;i<modes.size();++i) {
        c::Document doc({{32,24},96});
        auto base=raster("Backdrop",{32,24},{40,130,210,180});doc.insertLayer(0,base);
        auto l=raster("Blend",{24,20},{210,80,20,150},{3,2});l.blendMode=modes[i];l.opacity=.6F;
        auto fx=std::make_shared<c::LayerEffectStack>();fx->items[5].enabled=true;fx->items[5].blendMode=modes[i];
        l.effects=fx;doc.insertLayer(1,l);
        const auto path=dir+"/blend-v3-"+QString::fromUtf8(c::blendModeId(modes[i]))+".psd";
        CHECK(write(doc,path));auto parsed=parse(path);if(!parsed)continue;
        const auto record=std::ranges::find_if(parsed->records,[](const auto& r){return r.name=="Blend";});
        CHECK(record!=parsed->records.end());if(record==parsed->records.end())continue;
        CHECK(record->blend==keys[i]);
        CHECK(record->tags.value("lfx2").contains(descriptors[i]));
        auto job=std::make_shared<u::PsdJob>();auto inspected=u::inspectPsd(path,job);
        auto options=u::defaultPsdOptions(inspected);
        auto imported=u::convertPsd(inspected,options,job,{},false);CHECK(imported.document);
        if(imported.document) {
            CHECK(imported.document->layers().back().blendMode==modes[i]);
            CHECK(imported.document->layers().back().effects->items[5].blendMode==modes[i]);
        }
        c::Document ramps({{256,80},96});
        std::vector<std::byte> lower(256*80*4),upper(lower.size());
        for(int y=0;y<80;++y)for(int x=0;x<256;++x) {
            c::Rgba8 b{std::uint8_t(x),std::uint8_t(x),std::uint8_t(x),255};
            const auto v=std::uint8_t(y<16?127:y<32?128:255-x);
            c::Rgba8 s{v,v,v,255};
            if(y>=48&&y<64) {b={255,0,0,255};s={0,64,255,255};}
            if(y>=56&&y<64) {b={11,0,0,255};s={0,0,30,255};}
            if(y>=64) {b={40,130,210,128};s={210,80,20,std::uint8_t(x)};}
            const auto n=std::size_t(y*256+x)*4;
            const std::array a{b.red,b.green,b.blue,b.alpha},c{s.red,s.green,s.blue,s.alpha};
            for(std::size_t k=0;k<4;++k){lower[n+k]=std::byte(a[k]);upper[n+k]=std::byte(c[k]);}
        }
        auto back=c::Layer::raster("Backdrop ramp / red / half-alpha",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{256,80},std::move(lower)));
        auto front=c::Layer::raster("127 / 128 / complement / blue-green / alpha ramp",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{256,80},std::move(upper)));
        front.blendMode=modes[i];front.blendSeed=12345;
        ramps.insertLayer(0,back);ramps.insertLayer(1,front);
        const auto fixture=dir+"/reference-"+QString::fromUtf8(c::blendModeId(modes[i]));
        CHECK(write(ramps,fixture+".psd"));
        const auto rendered=u::flattenDocument(ramps);CHECK(rendered);
        if(rendered) {
            CHECK(rendered.image.save(fixture+".png"));
            const int x=12+int(i%2)*280,y=8+int(i/2)*112;
            painter.drawText(x,y+14,QString::fromUtf8(c::blendModeName(modes[i])));
            painter.drawImage(x,y+22,rendered.image);
        }
    }
    painter.drawText(12,478,"Rows: 127 / 128 / complementary ramps; weighted-color probes;");
    painter.drawText(12,496,"exact weighted tie; source-alpha ramp over half-alpha backdrop.");
    painter.drawText(12,520,"Native references; PSDs alongside are for independent redraw/edit checks.");
    painter.end();CHECK(sheet.save(dir+"/reference-contact-sheet.png"));
}
