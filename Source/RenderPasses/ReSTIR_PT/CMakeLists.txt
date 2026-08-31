add_plugin(ReSTIR_PT)

target_sources(ReSTIR_PT PRIVATE
    ReSTIR_PT.cpp
    ReSTIR_PT.h
	NextEventEstimationHelper.slang
	StructsAndHelpers.slang
	TraceCamera.rt.slang
	Resample.cs.slang
	EvaluateReservoirs.cs.slang
)

target_copy_shaders(ReSTIR_PT RenderPasses/ReSTIR_PT)
target_source_group(ReSTIR_PT "RenderPasses")
