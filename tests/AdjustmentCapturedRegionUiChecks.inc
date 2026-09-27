// Included after the UI fixture. The region is view state, not another mask edit.
void capturedRegionViewState()
{
    Fixture f;
    const auto originalSurface=std::get<c::RasterLayer>(f.document().layer(f.id)->payload).surface;
    const auto originalRevision=originalSurface->revision();
    const c::AffineTransform atCapture {1.1,.2,5,-.1,.9,3};
    CHECK(f.document().setLayerTransform(f.id,atCapture));
    auto mask=c::SelectionMask::rectangle({80,64},{10,12,42,30},20);
    mask=mask->combined(*c::SelectionMask::rectangle({80,64},{18,18,8,9}),c::SelectionOperation::Subtract);
    CHECK(f.document().setSelection(mask));f.select(f.id);f.page(0,0);
    f.widget<QPushButton>("AdjustmentCapture0")->click();settle();
    auto* toggle=f.widget<QCheckBox>("AdjustmentShowRegion0");
    CHECK(toggle->isVisible()&&toggle->isChecked());
    CHECK(f.canvas->scene().capturedRegionEdges);
    if(!f.canvas->scene().capturedRegionEdges)return;
    CHECK(f.canvas->scene().capturedRegionEdges->size()==mask->nonzeroBoundaryEdges().size());
    const auto captured=f.state().items[0].mask->coverage;
    CHECK(captured==mask);
    CHECK(f.document().setSelection({}));f.select(f.id);
    CHECK(!f.canvas->scene().selectionEdges || f.canvas->scene().selectionEdges->empty());
    CHECK(!f.canvas->selectionAnimationActive());
    CHECK(f.canvas->scene().capturedRegionEdges); // Deselect does not hide frozen scope.
    const auto edges=f.canvas->scene().capturedRegionEdges;
    const auto edgeRevision=f.canvas->scene().capturedRegionRevision;
    f.number("AdjustmentExposure")->setValue(.6);settle();
    CHECK(f.canvas->scene().capturedRegionEdges==edges);
    CHECK(f.canvas->scene().capturedRegionRevision==edgeRevision);
    f.history(false); // Preserve an existing redo branch during all view changes.
    const auto state=f.document().contentState();
    const auto undo=f.session().history().undoDepth(),redo=f.session().history().redoDepth();
    toggle->click();settle();CHECK(!f.canvas->scene().capturedRegionEdges);
    f.page(0,3);CHECK(!f.canvas->scene().capturedRegionEdges);
    f.page(0,0);CHECK(!toggle->isChecked()&&!f.canvas->scene().capturedRegionEdges);
    toggle->click();settle();CHECK(f.canvas->scene().capturedRegionEdges);
    f.page(1,0);CHECK(!f.canvas->scene().capturedRegionEdges);
    f.page(0,0);CHECK(f.canvas->scene().capturedRegionEdges);
    auto* shell=f.widget<QWidget>("AdjustmentsPanelShell");
    shell->hide();settle();CHECK(!f.canvas->scene().capturedRegionEdges);
    shell->show();settle();CHECK(f.canvas->scene().capturedRegionEdges);
    CHECK(f.document().contentState()==state);
    CHECK(f.session().history().undoDepth()==undo&&f.session().history().redoDepth()==redo);
    CHECK(originalSurface->revision()==originalRevision);

    // Moving, rotating and flipping layer geometry carries the owned mask.
    const c::AffineTransform now {-.8,.5,62,.3,1.2,-4};
    CHECK(f.document().setLayerTransform(f.id,now));
    f.canvas->setDocument(f.document().snapshot(),false);
    const auto mapping=c::composeAffine(now,*atCapture.inverted());
    const auto& expected=mask->nonzeroBoundaryEdges();
    const auto transformed=f.canvas->scene().capturedRegionEdges;
    CHECK(transformed&&transformed->size()==expected.size());
    if(transformed&&transformed->size()==expected.size())for(std::size_t i=0;i<expected.size();++i){
        CHECK((*transformed)[i].from==mapping.map(expected[i].from));
        CHECK((*transformed)[i].to==mapping.map(expected[i].to));
    }
    const auto transformedRevision=f.canvas->scene().capturedRegionRevision;
    QWheelEvent wheel(QPointF(150,150),QPointF(f.canvas->mapToGlobal(QPoint(150,150))),{},QPoint(0,120),
        Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(f.canvas,&wheel);
    CHECK(f.canvas->scene().capturedRegionRevision==transformedRevision);
    auto other=c::Layer::raster("Unmasked",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{8,8}));
    const auto otherId=other.id;CHECK(f.session().execute(std::make_unique<c::AddLayerCommand>(std::move(other),1)));
    f.select(otherId);CHECK(!f.canvas->scene().capturedRegionEdges);
    f.select(f.id);CHECK(f.canvas->scene().capturedRegionEdges);
    f.widget<QPushButton>("AdjustmentReset0")->click();settle();CHECK(!f.canvas->scene().capturedRegionEdges);
    f.history(false);CHECK(f.canvas->scene().capturedRegionEdges);
    CHECK(f.document().setSelection(c::SelectionMask::filled({80,64},0)));f.select(f.id);
    f.widget<QPushButton>("AdjustmentCapture0")->click();settle();
    CHECK(toggle->isVisible()&&!toggle->isEnabled());CHECK(!f.canvas->scene().capturedRegionEdges);
}
