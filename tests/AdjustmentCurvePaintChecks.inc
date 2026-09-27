// Include inside AdjustmentUiTests.cpp's anonymous namespace after CHECK and
// settle() are defined, then call curveStrokePreservesHistogramAndGrid().
void curveStrokePreservesHistogramAndGrid()
{
    u::AdjustmentCurveEditor editor;
    editor.resize(340,190);
    u::AdjustmentHistogram histogram {};
    // A flat nonzero histogram paints the complete plot with a translucent
    // tint; grid lines remain visible through it. Filling an arched curve with
    // the background brush would erase both underneath its implicit closure.
    for(auto& channel:histogram)channel.fill(100.);
    editor.setHistogram(histogram,0);
    editor.setPoints({{0,0},{1,1}});
    editor.evaluate=[](double x){return x;};
    editor.show();settle();
    const QImage baseline=editor.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    CHECK(!baseline.isNull());
    if(baseline.isNull())return;

    const auto verifyUncoveredPixels=[&](const c::CurveChannel& channel){
        std::vector<c::Vec2d> points;
        for(const auto& point:channel.points)points.push_back({point.input,point.output});
        editor.setPoints(points);
        editor.evaluate=[channel](double x){return c::evaluateCurve(channel,float(x));};
        editor.update();settle();
        const QImage rendered=editor.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        CHECK(rendered.size()==baseline.size());
        if(rendered.size()!=baseline.size())return;

        // This rectangle lies strictly between the identity reference and
        // each edited curve, comfortably away from strokes and point handles.
        // It includes the x=.5 and y=.75 grid lines and ordinary histogram fill.
        const double sx=double(rendered.width())/editor.width();
        const double sy=double(rendered.height())/editor.height();
        const double plotWidth=editor.width()-20.,plotHeight=editor.height()-20.;
        const int left=int(std::ceil((10.+plotWidth*.40)*sx));
        const int right=int(std::floor((10.+plotWidth*.60)*sx));
        const int top=int(std::ceil((10.+plotHeight*(1.-.76))*sy));
        const int bottom=int(std::floor((10.+plotHeight*(1.-.68))*sy));
        std::size_t compared=0,changed=0,nonBackground=0;
        const auto background=u::themeColor(u::ThemeColor::Background).rgb();
        for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){
            ++compared;
            if(rendered.pixel(x,y)!=baseline.pixel(x,y))++changed;
            if(baseline.pixelColor(x,y).rgb()!=background)++nonBackground;
        }
        CHECK(compared>100);
        CHECK(nonBackground==compared); // Actual histogram data, not blank pixels.
        CHECK(changed==0);
    };
    // Test changed post-construction geometry, including a non-monotonic curve.
    verifyUncoveredPixels(c::CurveChannel{{{0,0},{.30,.85},{.70,.95},{1,1}}});
    verifyUncoveredPixels(c::CurveChannel{{{0,0},{.35,.94},{.65,.84},{1,1}}});
    editor.close();settle();
}
