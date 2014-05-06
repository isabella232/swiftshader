// SwiftShader Software Renderer
//
// Copyright(c) 2005-2011 TransGaming Inc.
//
// All rights reserved. No part of this software may be copied, distributed, transmitted,
// transcribed, stored in a retrieval system, translated into any human or computer
// language by any means, or disclosed to third parties without the explicit written
// agreement of TransGaming Inc. Without such an agreement, no rights or licenses, express
// or implied, including but not limited to any patent rights, are granted to you.
//

#include "SetupProcessor.hpp"

#include "SetupRoutine.hpp"
#include "Primitive.hpp"
#include "Polygon.hpp"
#include "Viewport.hpp"
#include "Context.hpp"
#include "Renderer.hpp"
#include "Constants.hpp"
#include "Debug.hpp"

namespace sw
{
	extern bool complementaryDepthBuffer;

	unsigned int SetupProcessor::States::computeHash()
	{
		unsigned int *state = (unsigned int*)this;
		unsigned int hash = 0;

		for(int i = 0; i < sizeof(States) / 4; i++)
		{
			hash ^= state[i];
		}

		return hash;
	}

	SetupProcessor::State::State(int i)
	{
		memset(this, 0, sizeof(State));
	}

	bool SetupProcessor::State::operator==(const State &state) const
	{
		if(hash != state.hash)
		{
			return false;
		}

		return memcmp(static_cast<const States*>(this), static_cast<const States*>(&state), sizeof(States)) == 0;
	}

	SetupProcessor::SetupProcessor(Context *context) : context(context)
	{
		routineCache = 0;
		setRoutineCacheSize(1024);
	}

	SetupProcessor::~SetupProcessor()
	{
		delete routineCache;
		routineCache = 0;
	}

	SetupProcessor::State SetupProcessor::update() const
	{
		State state;

		state.isDrawPoint = context->isDrawPoint(true);
		state.isDrawLine = context->isDrawLine(true);
		state.isDrawTriangle = context->isDrawTriangle(false);
		state.isDrawSolidTriangle = context->isDrawTriangle(true);
		state.interpolateDepth = context->depthBufferActive() || context->pixelFogActive() != Context::FOG_NONE;
		state.perspective = context->perspectiveActive();
		state.pointSprite = context->pointSpriteActive();
		state.cullMode = context->cullMode;
		state.twoSidedStencil = context->stencilActive() && context->twoSidedStencil;
		state.slopeDepthBias = slopeDepthBias != 0.0f;
		state.vFace = context->pixelShader && context->pixelShader->vFaceDeclared;

		state.positionRegister = Pos;
		state.pointSizeRegister = 0xF;   // No vertex point size

		state.multiSample = context->renderTarget[0]->getMultiSampleCount();

		if(context->vertexShader)
		{
			state.positionRegister = context->vertexShader->positionRegister;
			state.pointSizeRegister = context->vertexShader->pointSizeRegister;
		}
		else if(context->pointSizeActive())
		{
			state.pointSizeRegister = Pts;
		}

		for(int interpolant = 0; interpolant < 11; interpolant++)
		{
			int componentCount = interpolant < 10 ? 4 : 1;   // Fog only has one component

			for(int component = 0; component < componentCount; component++)
			{
				state.gradient[interpolant][component].attribute = 0x3F;
				state.gradient[interpolant][component].flat = false;
				state.gradient[interpolant][component].wrap = false;
			}
		}

		const bool point = context->isDrawPoint(true);
		const bool sprite = context->pointSpriteActive();
		const bool flatShading = (context->shadingMode == Context::SHADING_FLAT) || point;

		if(context->vertexShader && context->pixelShader)
		{
			for(int interpolant = 0; interpolant < 10; interpolant++)
			{
				for(int component = 0; component < 4; component++)
				{
					int project = context->isProjectionComponent(interpolant - 2, component) ? 1 : 0;

					if(context->pixelShader->semantic[interpolant][component - project].active())
					{
						int input = interpolant;
						for(int i = 0; i < 12; i++)
						{
							if(context->pixelShader->semantic[interpolant][component - project] == context->vertexShader->output[i][component - project])
							{
								input = i;
								break;
							}
						}

						bool flat = point;

						switch(context->pixelShader->semantic[interpolant][component - project].usage)
						{
						case ShaderOperation::USAGE_TEXCOORD: flat = point && !sprite; break;
						case ShaderOperation::USAGE_COLOR:    flat = flatShading;      break;
						}

						state.gradient[interpolant][component].attribute = input;
						state.gradient[interpolant][component].flat = flat;
					}
				}
			}
		}
		else if(context->preTransformed && context->pixelShader)
		{
			for(int interpolant = 0; interpolant < 10; interpolant++)
			{
				for(int component = 0; component < 4; component++)
				{
					int index = context->pixelShader->semantic[interpolant][component].index;

					switch(context->pixelShader->semantic[interpolant][component].usage)
					{
					case 0xFF:
						break;
					case ShaderOperation::USAGE_TEXCOORD:
						state.gradient[interpolant][component].attribute = T0 + index;
						state.gradient[interpolant][component].flat = point && !sprite;
						break;
					case ShaderOperation::USAGE_COLOR:
						state.gradient[interpolant][component].attribute = D0 + index;
						state.gradient[interpolant][component].flat = flatShading;
						break;
					default:
						ASSERT(false);
					}
				}
			}
		}
		else if(context->pixelShaderVersion() < 0x0300)
		{
			for(int coordinate = 0; coordinate < 8; coordinate++)
			{
				for(int component = 0; component < 4; component++)
				{
					if(context->textureActive(coordinate, component))
					{
						state.texture[coordinate][component].attribute = T0 + coordinate;
						state.texture[coordinate][component].flat = point && !sprite;
						state.texture[coordinate][component].wrap = (context->textureWrap[coordinate] & (1 << component)) != 0;
					}
				}
			}

			for(int color = 0; color < 2; color++)
			{
				for(int component = 0; component < 4; component++)
				{
					if(context->colorActive(color, component))
					{
						state.color[color][component].attribute = D0 + color;
						state.color[color][component].flat = flatShading;
					}
				}
			}
		}
		else ASSERT(false);

		if(context->fogActive())
		{
			state.fog.attribute = Fog;
			state.fog.flat = point;
		}

		state.hash = state.computeHash();

		return state;
	}

	Routine *SetupProcessor::routine(const State &state)
	{
		Routine *routine = routineCache->query(state);

		if(!routine)
		{
			SetupRoutine *generator = new SetupRoutine(state);
			generator->generate();
			routine = generator->getRoutine();
			delete generator;

			routineCache->add(state, routine);
		}

		return routine;
	}

	void SetupProcessor::setRoutineCacheSize(int cacheSize)
	{
		delete routineCache;
		routineCache = new LRUCache<State, Routine>(clamp(cacheSize, 1, 65536));
	}
}