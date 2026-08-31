from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_ReSTIR_PT():
    g = RenderGraph('ReSTIR_PT')
    g.create_pass('ToneMapper', 'ToneMapper', {'outputSize': 'Default', 'useSceneMetadata': True, 'exposureCompensation': 0.0, 'autoExposure': False, 'filmSpeed': 100.0, 'whiteBalance': False, 'whitePoint': 6500.0, 'operator': 'Aces', 'clamp': True, 'whiteMaxLuminance': 1.0, 'whiteScale': 11.199999809265137, 'fNumber': 1.0, 'shutter': 1.0, 'exposureMode': 'AperturePriority'})
    g.create_pass('VBufferRT', 'VBufferRT', {'outputSize': 'Default', 'samplePattern': 'Center', 'sampleCount': 16, 'useAlphaTest': True, 'adjustShadingNormals': True, 'forceCullMode': False, 'cull': 'Back', 'cullNonOpaque': False, 'useTraceRayInline': False, 'useDOF': True})
    g.create_pass('ReSTIR_PT', 'ReSTIR_PT', {})
    g.create_pass('AccumulatePass', 'AccumulatePass', {'enabled': False, 'outputSize': 'Default', 'autoReset': True, 'precisionMode': 'Single', 'maxFrameCount': 0, 'overflowMode': 'Stop'})
    g.add_edge('AccumulatePass.output', 'ToneMapper.src')
    g.add_edge('ReSTIR_PT.color', 'AccumulatePass.input')
    g.add_edge('VBufferRT.vbuffer', 'ReSTIR_PT.vbuffer')
    g.add_edge('VBufferRT.viewW', 'ReSTIR_PT.view')
    g.add_edge('VBufferRT.mvec', 'ReSTIR_PT.mvec')
    g.mark_output('ToneMapper.dst')
    g.mark_output('AccumulatePass.output')
    return g

ReSTIR_PT = render_graph_ReSTIR_PT()
try: m.addGraph(ReSTIR_PT)
except NameError: None
