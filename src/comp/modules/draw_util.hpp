#pragma once

namespace comp
{
	// Vertex count backing a primitive count for the topologies NewDark uses.
	// Returns 0 for topologies the proxy does not reconstruct (points/lines).
	inline uint32_t vertex_count_for(const D3DPRIMITIVETYPE type, const UINT prim_count)
	{
		switch (type)
		{
		case D3DPT_TRIANGLEFAN:
		case D3DPT_TRIANGLESTRIP: return prim_count + 2;
		case D3DPT_TRIANGLELIST:  return prim_count * 3;
		default: return 0;
		}
	}
}
