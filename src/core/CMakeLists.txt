add_library(imageeditor_core STATIC
    src/Adjustments.cpp
    src/SpatialFilters.cpp
    src/AdjustmentCommands.cpp
    src/FilterCommands.cpp
    src/LayerEffects.cpp
    src/EffectCommands.cpp
    src/SpatialFilterCache.cpp
    src/RichText.cpp
    src/Shape.cpp
    src/ShapeCommands.cpp
    src/ShapeResize.cpp
    src/BasicPixelBrushEngine.cpp
    src/BasicPixelBrushStroke.cpp
    src/LocalBlurStroke.cpp
    src/CloneReference.cpp
    src/CloneStroke.cpp
    src/Healing.cpp
    src/SpotHealRepair.cpp
    src/BoundedParallel.cpp
    src/SpotHealStroke.cpp
    src/BrushAssetRegistry.cpp
    src/BrushPresetCodec.cpp
    src/BrushTip.cpp
    src/CreativeBrushes.cpp
    src/ColorSampler.cpp
    src/ColorSelection.cpp
    src/SmartSelectionReference.cpp
    src/QuickSelection.cpp
    src/MagicWandSelection.cpp
    src/Document.cpp
    src/DocumentCommands.cpp
    src/EditorSession.cpp
    src/EllipseCoverageRasterizer.cpp
    src/FillOperation.cpp
    src/FreehandSelectionPath.cpp
    src/AnchoredLassoPath.cpp
    src/LiveWire.cpp
    src/PolygonCoverageRasterizer.cpp
    src/RegionFinder.cpp
    src/History.cpp
    src/Layer.cpp
    src/LayerMask.cpp
    src/LayerMaskEdit.cpp
    src/LayerCrop.cpp
    src/LayerCropSession.cpp
    src/LayerTree.cpp
    src/LayerStructureCommands.cpp
    src/LayerCommands.cpp
    src/LayerTransform.cpp
    src/Measurement.cpp
    src/BoundsSnapping.cpp
    src/RulerTicks.cpp
    src/RasterSurface.cpp
    src/RegionalRasterSurface.cpp
    src/SelectedPixelTransform.cpp
    src/CoverageResampler.cpp
    src/RasterEditTransaction.cpp
    src/SelectionMask.cpp
    src/SelectionMaskManipulation.cpp
    src/SelectionCommands.cpp
    src/ViewportState.cpp
)

set_target_properties(imageeditor_core PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(imageeditor_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(imageeditor_core PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(imageeditor_core PUBLIC Threads::Threads)
imageeditor_enable_warnings(imageeditor_core)
