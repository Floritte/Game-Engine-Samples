#include "stdafx.h"
#include "DeferredRenderer.h"
#include "MaterialLibrary.h"

#include "Common/VertexArray.h"
#include "Common/VertexBuffer.h"
#include "Common/IndexBuffer.h"
#include "Common/Shader.h"
#include "Common/Common.h"

#include "Utils/glTFImporter.h"
#include "Utils/Utils.h"

#ifdef FROSTIUM_OPENGL_IMPL
#include "OpenGL/OpenglShader.h"
#include "OpenGL/OpenglRendererAPI.h"
#else
#include "Vulkan/VulkanPBR.h"
#endif

#ifdef FROSTIUM_SMOLENGINE_IMPL
#include "Extensions/JobsSystemInstance.h"
#endif

#include "Common/RendererStorage.h"
#include "Utils/glTFImporter.h"

#ifdef FROSTIUM_SMOLENGINE_IMPL
namespace SmolEngine
#else
namespace Frostium
#endif
{
	static RendererStorage* s_Data = nullptr;

	void DeferredRenderer::Init(RendererStorage* storage)
	{
		if (storage == nullptr)
		{
			throw std::runtime_error("Renderer: storage is nullptr!");
			abort();
		}

		s_Data = storage;
		{
			InitPBR();
			InitFramebuffers();
			InitPipelines();
		}

		s_Data->m_SceneData = &GraphicsContext::GetSingleton()->m_SceneData;
		s_Data->m_IsInitialized = true;
	}

	void DeferredRenderer::BeginScene(const ClearInfo* clearInfo)
	{
		bool mainCmd = true;
		s_Data->p_Gbuffer.BeginCommandBuffer(mainCmd);
		s_Data->p_Lighting.BeginCommandBuffer(mainCmd);
		s_Data->p_Combination.BeginCommandBuffer(mainCmd);
		s_Data->p_Skybox.BeginCommandBuffer(mainCmd);
		s_Data->p_DepthPass.BeginCommandBuffer(mainCmd);
		s_Data->p_FXAA.BeginCommandBuffer(mainCmd);
		s_Data->p_Bloom.BeginCommandBuffer(mainCmd);
		s_Data->p_Debug.BeginCommandBuffer(mainCmd);

		s_Data->p_Combination.BeginRenderPass();
		s_Data->p_Combination.ClearColors(clearInfo->color);
		s_Data->p_Combination.EndRenderPass();
		Reset();
	}

	void DeferredRenderer::EndScene()
	{
		Flush();
	}

	void DeferredRenderer::Flush()
	{
#ifdef FROSTIUM_SMOLENGINE_IMPL
		JobsSystemInstance::BeginSubmition();
#endif
		if (s_Data->m_UsedMeshes.size() > s_Data->m_DrawList.size())
		{
			s_Data->m_DrawList.resize(s_Data->m_UsedMeshes.size());
		}

		uint32_t cmdCount = static_cast<uint32_t>(s_Data->m_UsedMeshes.size());
		for (uint32_t i = 0; i < cmdCount; ++i)
		{
			// Getting values
			Mesh* mesh = s_Data->m_UsedMeshes[i];
			InstancePackage& instance = s_Data->m_Packages[mesh];
			CommandBuffer& cmd = s_Data->m_DrawList[i];

			// Setting draw list command
			cmd.Offset = s_Data->m_InstanceDataIndex;
			cmd.Mesh = mesh;
			cmd.InstancesCount = instance.CurrentIndex;

			for (uint32_t x = 0; x < instance.CurrentIndex; ++x)
			{
				InstancePackage::Package& package = instance.Packages[x];
				InstanceData& instanceUBO = s_Data->m_InstancesData[s_Data->m_InstanceDataIndex];
				AnimationProperties* props = nullptr;

				uint32_t animStartOffset = 0;
				const bool animated = mesh->IsAnimated();
				bool animActive = false;

				// Animations
				{
					for (uint32_t y = 0; y < mesh->GetAnimationsCount(); ++y)
					{
						props = mesh->GetAnimationProperties(y);
						if (props->IsActive())
						{
							animActive = true;
							break;
						}
					}

					if (animated && mesh->m_ImportedData)
					{
						ImportedDataGlTF* imported = mesh->m_ImportedData;
						glTFAnimation* activeAnim = &imported->Animations[imported->ActiveAnimation];
						if (mesh->IsRootNode())
						{
							animStartOffset = s_Data->m_LastAnimationOffset;
							s_Data->m_RootOffsets[mesh] = animStartOffset;

							if (animActive)
							{
								mesh->UpdateAnimations();
							}

							uint32_t jointCount = static_cast<uint32_t>(props->Joints.size());
							for (uint32_t x = 0; x < jointCount; ++x)
							{
								s_Data->m_AnimationJoints[s_Data->m_LastAnimationOffset] = props->Joints[x];
								s_Data->m_LastAnimationOffset++;
							}
						}

						if (animated && !mesh->IsRootNode())
						{
							auto& it = s_Data->m_RootOffsets.find(mesh->m_Root);
							if (it != s_Data->m_RootOffsets.end())
								animStartOffset = it->second;
						}
					}
				}

				// Transform
				{
#ifdef FROSTIUM_SMOLENGINE_IMPL
					JobsSystemInstance::Schedule([animated, animStartOffset, &package, &instanceUBO]()
						{
							Utils::ComposeTransform(*package.WorldPos, *package.Rotation, *package.Scale, instanceUBO.ModelView);

							instanceUBO.MaterialID = package.MaterialID;
							instanceUBO.IsAnimated = animated;
							instanceUBO.AnimOffset = animStartOffset;
							instanceUBO.EntityID = 0; // temp
					});
#else
					Utils::ComposeTransform(*package.WorldPos, *package.Rotation, *package.Scale, instanceUBO.ModelView);

					instanceUBO.MaterialID = package.MaterialID;
					instanceUBO.IsAnimated = animated;
					instanceUBO.AnimOffset = animStartOffset;
					instanceUBO.EntityID = 0; // temp
#endif
				}

				s_Data->m_InstanceDataIndex++;
			}

			instance.CurrentIndex = 0;
		}

#ifdef FROSTIUM_SMOLENGINE_IMPL
		JobsSystemInstance::EndSubmition();
#endif

		// Updates UBOs and SSBOs 
		{
#ifdef FROSTIUM_SMOLENGINE_IMPL

			JobsSystemInstance::BeginSubmition();
			{
				// Updates Scene Data
				JobsSystemInstance::Schedule([]()
				{
					s_Data->p_Gbuffer.SubmitBuffer(s_Data->m_SceneDataBinding, sizeof(SceneData), s_Data->m_SceneData);
				});

				// Updates Lighting State
				JobsSystemInstance::Schedule([]()
				{
					s_Data->m_State.SceneLighting.NumPointsLights = s_Data->m_PointLightIndex;
					s_Data->m_State.SceneLighting.NumSpotLights = s_Data->m_SpotLightIndex;
					s_Data->p_Lighting.SubmitBuffer(s_Data->m_LightingStateBinding, sizeof(LightingProperties), &s_Data->m_State.SceneLighting);
				});

				// Updates Bloom State
				JobsSystemInstance::Schedule([]()
				{
					s_Data->p_Lighting.SubmitBuffer(s_Data->m_BloomStateBinding, sizeof(BloomProperties), &s_Data->m_State.SceneBloom);
				});

				// Updates Directional Lights
				JobsSystemInstance::Schedule([]()
				{
					s_Data->p_Lighting.SubmitBuffer(s_Data->m_DirLightBinding, sizeof(DirectionalLight), &s_Data->m_DirLight);
					if (s_Data->m_DirLight.IsCastShadows)
					{
						// Calculate Depth
						CalculateDepthMVP(s_Data->m_MainPushConstant.DepthMVP);
					}
				});

				// Updates Point Lights
				JobsSystemInstance::Schedule([]()
				{
					if (s_Data->m_PointLightIndex > 0)
					{
						s_Data->p_Lighting.SubmitBuffer(s_Data->m_PointLightBinding,
							sizeof(PointLight) * s_Data->m_PointLightIndex, s_Data->m_PointLights.data());
					}
				});

				// Updates Spot Lights
				JobsSystemInstance::Schedule([]()
				{
					if (s_Data->m_SpotLightIndex > 0)
					{
						s_Data->p_Lighting.SubmitBuffer(s_Data->m_SpotLightBinding,
							sizeof(SpotLight) * s_Data->m_SpotLightIndex, s_Data->m_SpotLights.data());
		            }
			    });

				// Updates Animation joints
				JobsSystemInstance::Schedule([]()
				{
					if (s_Data->m_LastAnimationOffset > 0)
					{
						s_Data->p_Gbuffer.SubmitBuffer(s_Data->m_AnimBinding,
							sizeof(glm::mat4) * s_Data->m_LastAnimationOffset, s_Data->m_AnimationJoints.data());
					}
				});

				// Updates Batch Data
				JobsSystemInstance::Schedule([]()
				{
					if (s_Data->m_InstanceDataIndex > 0)
					{
						s_Data->p_Gbuffer.SubmitBuffer(s_Data->m_ShaderDataBinding,
							sizeof(InstanceData) * s_Data->m_InstanceDataIndex, s_Data->m_InstancesData.data());
					}
				});

			}
			JobsSystemInstance::EndSubmition();
#else
			// Updates scene data
			s_Data->m_GbufferPipeline.SubmitBuffer(s_Data->m_SceneDataBinding, sizeof(SceneData), s_Data->m_SceneData);

			// Updates scene state
			{
				s_Data->m_State.SceneState.NumPointsLights = s_Data->m_PointLightIndex;
				s_Data->m_State.SceneState.NumSpotLights = s_Data->m_SpotLightIndex;
				s_Data->m_LightingPipeline.SubmitBuffer(s_Data->m_SceneStateBinding, sizeof(SceneState), &s_Data->m_State.SceneState);
			}

			// Updates Directional Lights
			s_Data->m_LightingPipeline.SubmitBuffer(s_Data->m_DirLightBinding, sizeof(DirectionalLight), &s_Data->m_DirLight);
			if (s_Data->m_DirLight.IsCastShadows)
			{
				// Calculate Depth
				CalculateDepthMVP(s_Data->m_MainPushConstant.DepthMVP);
			}

			// Updates Point Lights
			if (s_Data->m_PointLightIndex > 0)
			{
				s_Data->m_LightingPipeline.SubmitBuffer(s_Data->m_PointLightBinding, sizeof(PointLight) * s_Data->m_PointLightIndex, s_Data->m_PointLights.data());
			}

			// Updates Spot Lights
			if (s_Data->m_SpotLightIndex > 0)
			{
				s_Data->m_LightingPipeline.SubmitBuffer(s_Data->m_SpotLightBinding, sizeof(SpotLight) * s_Data->m_SpotLightIndex, s_Data->m_SpotLights.data());
			}

			// Updates Animation joints
			if (s_Data->m_LastAnimationOffset > 0)
			{
				s_Data->m_GbufferPipeline.SubmitBuffer(s_Data->m_AnimBinding, sizeof(glm::mat4) * s_Data->m_LastAnimationOffset, s_Data->m_AnimationJoints.data());
			}

			// Updates Batch Data
			if (s_Data->m_InstanceDataIndex > 0)
			{
				s_Data->m_GbufferPipeline.SubmitBuffer(s_Data->m_ShaderDataBinding, sizeof(InstanceData) * s_Data->m_InstanceDataIndex, s_Data->m_InstancesData.data());
			}
#endif
		}

		// Depth Pass
		if (s_Data->m_DirLight.IsActive && s_Data->m_DirLight.IsCastShadows)
		{
#ifndef FROSTIUM_OPENGL_IMPL
			VkCommandBuffer cmdBuffer = s_Data->p_DepthPass.GetVkCommandBuffer();
#endif
			s_Data->p_DepthPass.BeginRenderPass();
			{
#ifndef FROSTIUM_OPENGL_IMPL
				// Set depth bias (aka "Polygon offset")
				// Required to avoid shadow mapping artifacts
				vkCmdSetDepthBias(cmdBuffer, 1.25f, 0.0f, 1.75f);
#endif
				struct PushConstant
				{
					glm::mat4 depthMVP;
					uint32_t offset;

				} static pc;

				pc.depthMVP = s_Data->m_MainPushConstant.DepthMVP;
				for (uint32_t i = 0; i < cmdCount; ++i)
				{
					auto& cmd = s_Data->m_DrawList[i];
					pc.offset = cmd.Offset;

					s_Data->p_DepthPass.SubmitPushConstant(ShaderType::Vertex, sizeof(PushConstant), &pc);
					s_Data->p_DepthPass.DrawMeshIndexed(cmd.Mesh, cmd.InstancesCount);
				}
			}
			s_Data->p_DepthPass.EndRenderPass();
		}

		// G-Buffer Pass
		s_Data->p_Gbuffer.BeginRenderPass();
		{
			// SkyBox
			if (s_Data->m_State.bDrawSkyBox)
			{
				s_Data->p_Skybox.Draw(36);
			}

			// Grid
			if (s_Data->m_State.bDrawGrid)
			{
				s_Data->p_Grid.BeginCommandBuffer(true);
				s_Data->p_Grid.SubmitPushConstant(ShaderType::Vertex, sizeof(glm::mat4), &s_Data->m_GridModel);
				s_Data->p_Grid.DrawMeshIndexed(&s_Data->m_PlaneMesh);
			}

			for (uint32_t i = 0; i < cmdCount; ++i)
			{
				auto& cmd = s_Data->m_DrawList[i];

				s_Data->m_MainPushConstant.DataOffset = cmd.Offset;
				s_Data->p_Gbuffer.SubmitPushConstant(ShaderType::Vertex, s_Data->m_PushConstantSize, &s_Data->m_MainPushConstant);
				s_Data->p_Gbuffer.DrawMeshIndexed(cmd.Mesh, cmd.InstancesCount);
			}
		}
		s_Data->p_Gbuffer.EndRenderPass();

		// Debug View
		if (s_Data->m_State.eDebugView != DebugViewFlags::None)
		{
			s_Data->p_Debug.BeginRenderPass();
			{
				uint32_t state = (uint32_t)s_Data->m_State.eDebugView;
				s_Data->p_Debug.SubmitPushConstant(ShaderType::Fragment, sizeof(uint32_t), &state);
				s_Data->p_Debug.Draw(3);
			}
			s_Data->p_Debug.EndRenderPass();

			return;
		}

		// Lighting Pass
		{
			s_Data->p_Lighting.BeginRenderPass();
			{
				s_Data->p_Lighting.Draw(3);
			}
			s_Data->p_Lighting.EndRenderPass();
		}

		// Post-Processing: FXAA, Bloom, Blur
		{
			// FXAA
			{
				s_Data->p_FXAA.BeginRenderPass();
				{
					struct Info
					{
						glm::vec2 screenSize;
						uint32_t fxaaEnabled;
					} info;

					float width = 1.0f / static_cast<float>(s_Data->f_FXAA.GetSpecification().Width);
					float height = 1.0f / static_cast<float>(s_Data->f_FXAA.GetSpecification().Height);
					info.screenSize = glm::vec2(width, height);
					info.fxaaEnabled = s_Data->m_State.bFXAA;

					s_Data->p_FXAA.SubmitPushConstant(ShaderType::Fragment, sizeof(Info), &info);
					s_Data->p_FXAA.Draw(3);

				}
				s_Data->p_FXAA.EndRenderPass();
			}

			// Bloom or Blur
			{
				if (s_Data->m_State.bBloom)
				{
					s_Data->p_Bloom.BeginRenderPass();
					{
						s_Data->p_Bloom.Draw(3);
					}
					s_Data->p_Bloom.EndRenderPass();
				}
			}

			// Composition
			{
				s_Data->p_Combination.BeginRenderPass();
				{
					struct push_constant
					{
						uint32_t state;
						uint32_t vertical_blur_enabled;
					} pc;

					pc.state = s_Data->m_State.bBloom ? 1: 0;
					pc.vertical_blur_enabled = s_Data->m_State.bVerticalBloom;

					s_Data->p_Combination.SubmitPushConstant(ShaderType::Fragment, sizeof(push_constant), &pc);
					s_Data->p_Combination.Draw(3);
				}
				s_Data->p_Combination.EndRenderPass();
			}
		}
	}

	void DeferredRenderer::StartNewBacth()
	{
		Flush();
		Reset();
	}

	void DeferredRenderer::SubmitMesh(const glm::vec3& pos, const glm::vec3& rotation,
		const glm::vec3& scale, Mesh* mesh, const uint32_t& materialID)
	{
		if (s_Data->m_Frustum->CheckSphere(pos, 10.0f) && mesh != nullptr)
		{
			AddMesh(pos, rotation, scale, mesh, materialID == 0 ? mesh->m_MaterialID: materialID);
		}
	}

	void DeferredRenderer::SubmitMeshEx(const glm::vec3& pos, const glm::vec3& rotation, const glm::vec3& scale, Mesh* mesh, const uint32_t& PBRmaterialID)
	{
		if (s_Data->m_Frustum->CheckSphere(pos, 10.0f) && mesh != nullptr)
		{
			if (s_Data->m_Objects >= max_objects)
				StartNewBacth();

			auto& instance = s_Data->m_Packages[mesh];
			InstancePackage::Package* package = nullptr;

			if (instance.CurrentIndex == instance.Packages.size())
			{
				instance.Packages.emplace_back(InstancePackage::Package());
				package = &instance.Packages.back();
			}
			else
				package = &instance.Packages[instance.CurrentIndex];

			package->MaterialID = PBRmaterialID;
			package->WorldPos = const_cast<glm::vec3*>(&pos);
			package->Rotation = const_cast<glm::vec3*>(&rotation);
			package->Scale = const_cast<glm::vec3*>(&scale);

			instance.CurrentIndex++;
			s_Data->m_Objects++;

			bool found = std::find(s_Data->m_UsedMeshes.begin(), s_Data->m_UsedMeshes.end(), mesh) != s_Data->m_UsedMeshes.end();
			if (found == false)
				s_Data->m_UsedMeshes.emplace_back(mesh);
		}
	}

	void DeferredRenderer::SubmitDirLight(DirectionalLight* light)
	{
		assert(light != nullptr);
		s_Data->m_DirLight = *light;
	}

	void DeferredRenderer::SubmitPointLight(PointLight* light)
	{
		uint32_t index = s_Data->m_PointLightIndex;
		if (index >= max_lights || light == nullptr)
			return;

		s_Data->m_PointLights[index] = *light;
		s_Data->m_PointLightIndex++;
	}

	void DeferredRenderer::SubmitSpotLight(SpotLight* light)
	{
		uint32_t index = s_Data->m_SpotLightIndex;
		if (index >= max_lights || light == nullptr)
			return;

		s_Data->m_SpotLights[index] = *light;
		s_Data->m_SpotLightIndex++;
	}

	void DeferredRenderer::SetRendererState(RendererState* state)
	{
		assert(state != nullptr);
		s_Data->m_State = *state;
	}

	void DeferredRenderer::InitPBR()
	{
#ifdef FROSTIUM_OPENGL_IMPL
#else
		VulkanPBR::Init(GraphicsContext::s_Instance->m_ResourcesFolderPath + "Skyboxes/uffizi_cube.ktx", TextureFormat::R16G16B16A16_SFLOAT);
#endif
	}

	void DeferredRenderer::InitPipelines()
	{
		float skyboxVertices[] = {
			// positions          
			-1.0f,  1.0f, -1.0f,
			-1.0f, -1.0f, -1.0f,
			 1.0f, -1.0f, -1.0f,
			 1.0f, -1.0f, -1.0f,
			 1.0f,  1.0f, -1.0f,
			-1.0f,  1.0f, -1.0f,

			-1.0f, -1.0f,  1.0f,
			-1.0f, -1.0f, -1.0f,
			-1.0f,  1.0f, -1.0f,
			-1.0f,  1.0f, -1.0f,
			-1.0f,  1.0f,  1.0f,
			-1.0f, -1.0f,  1.0f,

			 1.0f, -1.0f, -1.0f,
			 1.0f, -1.0f,  1.0f,
			 1.0f,  1.0f,  1.0f,
			 1.0f,  1.0f,  1.0f,
			 1.0f,  1.0f, -1.0f,
			 1.0f, -1.0f, -1.0f,

			-1.0f, -1.0f,  1.0f,
			-1.0f,  1.0f,  1.0f,
			 1.0f,  1.0f,  1.0f,
			 1.0f,  1.0f,  1.0f,
			 1.0f, -1.0f,  1.0f,
			-1.0f, -1.0f,  1.0f,

			-1.0f,  1.0f, -1.0f,
			 1.0f,  1.0f, -1.0f,
			 1.0f,  1.0f,  1.0f,
			 1.0f,  1.0f,  1.0f,
			-1.0f,  1.0f,  1.0f,
			-1.0f,  1.0f, -1.0f,

			-1.0f, -1.0f, -1.0f,
			-1.0f, -1.0f,  1.0f,
			 1.0f, -1.0f, -1.0f,
			 1.0f, -1.0f, -1.0f,
			-1.0f, -1.0f,  1.0f,
			 1.0f, -1.0f,  1.0f
		};

		BufferLayout mainLayout =
		{
			{ DataTypes::Float3, "aPos" },
			{ DataTypes::Float3, "aNormal" },
			{ DataTypes::Float3, "aTangent" },
			{ DataTypes::Float2, "aUV" },
			{ DataTypes::Int4,   "aBoneIDs"},
			{ DataTypes::Float4, "aWeight"}
		};

		VertexInputInfo vertexMain(sizeof(PBRVertex), mainLayout);

		// Gbuffer
		{
			GraphicsPipelineShaderCreateInfo shaderCI = {};
			{
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/Gbuffer.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Gbuffer.frag";

				// SSBO's
				ShaderBufferInfo bufferInfo = {};

				// Vertex
				bufferInfo.Size = sizeof(InstanceData) * max_objects;
				shaderCI.BufferInfos[s_Data->m_ShaderDataBinding] = bufferInfo;

				bufferInfo.Size = sizeof(PBRMaterial) * max_materials;
				shaderCI.BufferInfos[s_Data->m_MaterialsBinding] = bufferInfo;

				bufferInfo.Size = sizeof(glm::mat4) * max_anim_joints;
				shaderCI.BufferInfos[s_Data->m_AnimBinding] = bufferInfo;
			};

			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			{
				DynamicPipelineCI.VertexInputInfos = { vertexMain };
				DynamicPipelineCI.PipelineName = "G_Pipeline";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_GBuffer };
			}

			auto result = s_Data->p_Gbuffer.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
		}

		// Lighting
		{
			GraphicsPipelineShaderCreateInfo shaderCI = {};
			{
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Lighting.frag";

				ShaderBufferInfo bufferInfo = {};

				// Fragment
				bufferInfo.Size = sizeof(PointLight) * max_lights;
				shaderCI.BufferInfos[s_Data->m_PointLightBinding] = bufferInfo;

				bufferInfo.Size = sizeof(SpotLight) * max_lights;
				shaderCI.BufferInfos[s_Data->m_SpotLightBinding] = bufferInfo;

				bufferInfo.Size = sizeof(DirectionalLight);
				shaderCI.BufferInfos[s_Data->m_DirLightBinding] = bufferInfo;
			};

			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			{
				DynamicPipelineCI.PipelineName = "Lighting_Pipeline";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.bDepthTestEnabled = false;
				DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_Lighting };
			}

			auto result = s_Data->p_Lighting.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);

			s_Data->p_Lighting.UpdateSampler(&s_Data->f_Depth, 1, "Depth_Attachment");
#ifndef FROSTIUM_OPENGL_IMPL
			s_Data->p_Lighting.UpdateVulkanImageDescriptor(2, VulkanPBR::GetIrradianceImageInfo());
			s_Data->p_Lighting.UpdateVulkanImageDescriptor(3, VulkanPBR::GetBRDFLUTImageInfo());
			s_Data->p_Lighting.UpdateVulkanImageDescriptor(4, VulkanPBR::GetPrefilteredCubeImageInfo());
#endif
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 5, "albedro");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 6, "position");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 7, "normals");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 8, "materials");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 9, "emission");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 10, "shadow_coord");
		}

#ifdef FROSTIUM_SMOLENGINE_IMPL

		JobsSystemInstance::BeginSubmition();
		{
			// Grid
			JobsSystemInstance::Schedule([&]()
			{
					Utils::ComposeTransform(glm::vec3(0), glm::vec3(0), { 100, 1, 100 }, s_Data->m_GridModel);
					Mesh::Create(GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Models/plane_v2.gltf", &s_Data->m_PlaneMesh);

					GraphicsPipelineCreateInfo pipelineCI = {};
					GraphicsPipelineShaderCreateInfo shaderCI = {};
					{
						shaderCI.FilePaths[ShaderType::Vertex] = GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Shaders/Grid.vert";
						shaderCI.FilePaths[ShaderType::Fragment] = GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Shaders/Grid.frag";
					};

					pipelineCI.PipelineName = "Grid";
					pipelineCI.eCullMode = CullMode::None;
					pipelineCI.VertexInputInfos = { vertexMain };
					pipelineCI.bDepthTestEnabled = false;
					pipelineCI.bDepthWriteEnabled = false;
					pipelineCI.TargetFramebuffers = { &s_Data->f_GBuffer};
					pipelineCI.ShaderCreateInfo = shaderCI;
					assert(s_Data->p_Grid.Create(&pipelineCI) == PipelineCreateResult::SUCCESS);
			});

			// Skybox
			JobsSystemInstance::Schedule([&]()
			{
					GraphicsPipelineShaderCreateInfo shaderCI = {};
					{
						shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/Skybox.vert";
						shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Skybox.frag";
					};

					struct SkyBoxData
					{
						glm::vec3 pos;
					};

					BufferLayout layout =
					{
						{ DataTypes::Float3, "aPos" }
					};

					GraphicsPipelineCreateInfo DynamicPipelineCI = {};
					{
						DynamicPipelineCI.VertexInputInfos = { VertexInputInfo(sizeof(SkyBoxData), layout) };
						DynamicPipelineCI.PipelineName = "Skybox_Pipiline";
						DynamicPipelineCI.ShaderCreateInfo = shaderCI;
						DynamicPipelineCI.bDepthTestEnabled = false;
						DynamicPipelineCI.bDepthWriteEnabled = false;
						DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_GBuffer };
					}

					auto result = s_Data->p_Skybox.Create(&DynamicPipelineCI);
					assert(result == PipelineCreateResult::SUCCESS);
#ifndef FROSTIUM_OPENGL_IMPL
					s_Data->p_Skybox.UpdateVulkanImageDescriptor(1, VulkanPBR::GetSkyBox().GetVkDescriptorImageInfo());
#endif
					Ref<VertexBuffer> skyBoxFB = std::make_shared<VertexBuffer>();
					VertexBuffer::Create(skyBoxFB.get(), skyboxVertices, sizeof(skyboxVertices));
					s_Data->p_Skybox.SetVertexBuffers({ skyBoxFB });
			});


			// Depth Pass
			JobsSystemInstance::Schedule([&]()
			{
				GraphicsPipelineShaderCreateInfo shaderCI = {};
				{
					shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/DepthPass.vert";
					shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/DepthPass.frag";
				};

				GraphicsPipelineCreateInfo DynamicPipelineCI = {};
				{
					DynamicPipelineCI.VertexInputInfos = { vertexMain };
					DynamicPipelineCI.PipelineName = "DepthPass_Pipeline";
					DynamicPipelineCI.ShaderCreateInfo = shaderCI;
					DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_Depth };
					DynamicPipelineCI.bDepthBiasEnabled = true;
					DynamicPipelineCI.StageCount = 1;

					auto result = s_Data->p_DepthPass.Create(&DynamicPipelineCI);
					assert(result == PipelineCreateResult::SUCCESS);
				}
			});

			// FXAA
			JobsSystemInstance::Schedule([&]()
			{
				GraphicsPipelineCreateInfo DynamicPipelineCI = {};
				DynamicPipelineCI.eCullMode = CullMode::None;

				GraphicsPipelineShaderCreateInfo shaderCI = {};
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/FXAA.frag";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.PipelineName = "FXAA";
				DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_FXAA };

				auto result = s_Data->p_FXAA.Create(&DynamicPipelineCI);
				assert(result == PipelineCreateResult::SUCCESS);
				s_Data->p_FXAA.UpdateSampler(&s_Data->f_Lighting, 0);
			});

			// Combination
			JobsSystemInstance::Schedule([&]()
			{
				GraphicsPipelineCreateInfo DynamicPipelineCI = {};
				DynamicPipelineCI.eCullMode = CullMode::None;
				DynamicPipelineCI.TargetFramebuffers = { s_Data->f_Main };

				GraphicsPipelineShaderCreateInfo shaderCI = {};
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Combination.frag";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.PipelineName = "Combination";

				auto result = s_Data->p_Combination.Create(&DynamicPipelineCI);
				assert(result == PipelineCreateResult::SUCCESS);
				s_Data->p_Combination.UpdateSampler(&s_Data->f_FXAA, 0);
				s_Data->p_Combination.UpdateSampler(&s_Data->f_Bloom, 1);
			});

			// Debug
			JobsSystemInstance::Schedule([&]()
			{
				GraphicsPipelineCreateInfo DynamicPipelineCI = {};
				DynamicPipelineCI.eCullMode = CullMode::None;
				DynamicPipelineCI.TargetFramebuffers = { s_Data->f_Main };

				GraphicsPipelineShaderCreateInfo shaderCI = {};
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/DebugView.frag";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.PipelineName = "Debug";

				auto result = s_Data->p_Debug.Create(&DynamicPipelineCI);
				assert(result == PipelineCreateResult::SUCCESS);

				s_Data->p_Debug.UpdateSampler(&s_Data->f_Depth, 1, "Depth_Attachment");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 5, "albedro");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 6, "position");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 7, "normals");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 8, "materials");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 9, "emission");
				s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 10, "shadow_coord");
			});


			// Bloom
			JobsSystemInstance::Schedule([&]()
			{
				GraphicsPipelineCreateInfo DynamicPipelineCI = {};
				DynamicPipelineCI.eCullMode = CullMode::None;

				GraphicsPipelineShaderCreateInfo shaderCI = {};
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Bloom.frag";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.eColorBlendOp = BlendOp::ADD;
				DynamicPipelineCI.eSrcColorBlendFactor = BlendFactor::ONE;
				DynamicPipelineCI.eDstColorBlendFactor = BlendFactor::ONE;
				DynamicPipelineCI.eAlphaBlendOp = BlendOp::ADD;
				DynamicPipelineCI.eSrcAlphaBlendFactor = BlendFactor::SRC_ALPHA;
				DynamicPipelineCI.eDstAlphaBlendFactor = BlendFactor::DST_ALPHA;

				DynamicPipelineCI.TargetFramebuffers = { &s_Data->f_Bloom};
				DynamicPipelineCI.PipelineName = "Bloom";
				auto result = s_Data->p_Bloom.Create(&DynamicPipelineCI);
				assert(result == PipelineCreateResult::SUCCESS);

				s_Data->p_Bloom.UpdateSampler(&s_Data->f_Lighting, 0, "color_2");
				s_Data->p_Bloom.UpdateSampler(&s_Data->f_Lighting, 1, "color_3");
			});

		}

		JobsSystemInstance::EndSubmition();

#else
		// Grid
		{
			Utils::ComposeTransform(glm::vec3(0), glm::vec3(0), { 100, 1, 100 }, s_Data->m_GridModel);
			Mesh::Create(GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Models/plane_v2.gltf", &s_Data->m_PlaneMesh);

			GraphicsPipelineCreateInfo pipelineCI = {};
			GraphicsPipelineShaderCreateInfo shaderCI = {};
			{
				shaderCI.FilePaths[ShaderType::Vertex] = GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Shaders/Grid.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = GraphicsContext::GetSingleton()->m_ResourcesFolderPath + "Shaders/Grid.frag";
			};

			pipelineCI.PipelineName = "Grid";
			pipelineCI.eCullMode = CullMode::None;
			pipelineCI.VertexInputInfos = { vertexMain };
			pipelineCI.bDepthTestEnabled = false;
			pipelineCI.bDepthWriteEnabled = false;
			pipelineCI.pTargetFramebuffer = &s_Data->m_GFramebuffer;
			pipelineCI.ShaderCreateInfo = shaderCI;

			assert(s_Data->m_GridPipeline.Create(&pipelineCI) == PipelineCreateResult::SUCCESS);
		}

		// Sky Box
		{
			GraphicsPipelineShaderCreateInfo shaderCI = {};
			{
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/Skybox.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Skybox.frag";
			};

			struct SkyBoxData
			{
				glm::vec3 pos;
			};

			BufferLayout layout =
			{
				{ DataTypes::Float3, "aPos" }
			};

			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			{
				DynamicPipelineCI.VertexInputInfos = { VertexInputInfo(sizeof(SkyBoxData), layout) };
				DynamicPipelineCI.PipelineName = "Skybox_Pipiline";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.bDepthTestEnabled = false;
				DynamicPipelineCI.bDepthWriteEnabled = false;
				DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_GFramebuffer;
			}

			auto result = s_Data->m_SkyboxPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
#ifndef FROSTIUM_OPENGL_IMPL
			s_Data->m_SkyboxPipeline.UpdateVulkanImageDescriptor(1, VulkanPBR::GetSkyBox().GetVkDescriptorImageInfo());
#endif

			Ref<VertexBuffer> skyBoxFB = std::make_shared<VertexBuffer>();
			VertexBuffer::Create(skyBoxFB.get(), skyboxVertices, sizeof(skyboxVertices));
			s_Data->m_SkyboxPipeline.SetVertexBuffers({ skyBoxFB });
		}

		// Depth Pass
		{
			GraphicsPipelineShaderCreateInfo shaderCI = {};
			{
				shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/DepthPass.vert";
				shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/DepthPass.frag";
			};

			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			{
				DynamicPipelineCI.VertexInputInfos = { vertexMain };
				DynamicPipelineCI.PipelineName = "DepthPass_Pipeline";
				DynamicPipelineCI.ShaderCreateInfo = shaderCI;
				DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_DepthFramebuffer;
				DynamicPipelineCI.bDepthBiasEnabled = true;
				DynamicPipelineCI.StageCount = 1;

				auto result = s_Data->m_DepthPassPipeline.Create(&DynamicPipelineCI);
				assert(result == PipelineCreateResult::SUCCESS);
			}
		}

		// HDR
		{
			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			DynamicPipelineCI.eCullMode = CullMode::None;

			GraphicsPipelineShaderCreateInfo shaderCI = {};
			shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
			shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/HDR.frag";
			DynamicPipelineCI.ShaderCreateInfo = shaderCI;
			DynamicPipelineCI.PipelineName = "HDR";
			DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_HDRFramebuffer;

			auto result = s_Data->m_HDRPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
			s_Data->m_HDRPipeline.UpdateSampler(&s_Data->m_LightingFramebuffer, 0);
		}

		// Combination
		{
			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			DynamicPipelineCI.eCullMode = CullMode::None;
			DynamicPipelineCI.pTargetFramebuffer = s_Data->m_MainFramebuffer;

			GraphicsPipelineShaderCreateInfo shaderCI = {};
			shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
			shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Combination.frag";
			DynamicPipelineCI.ShaderCreateInfo = shaderCI;
			DynamicPipelineCI.PipelineName = "Combination";

			auto result = s_Data->m_CombinationPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
			s_Data->m_CombinationPipeline.UpdateSampler(&s_Data->m_HDRFramebuffer, 0);
			s_Data->m_CombinationPipeline.UpdateSampler(&s_Data->m_PostProcessingFramebuffer, 1);
		}

		// Debug
		{
			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			DynamicPipelineCI.eCullMode = CullMode::None;
			DynamicPipelineCI.pTargetFramebuffer = s_Data->m_MainFramebuffer;

			GraphicsPipelineShaderCreateInfo shaderCI = {};
			shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
			shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/DebugView.frag";
			DynamicPipelineCI.ShaderCreateInfo = shaderCI;
			DynamicPipelineCI.PipelineName = "Debug";

			auto result = s_Data->m_DebugPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);

			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_DepthFramebuffer, 1, "Depth_Attachment");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 5, "albedro");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 6, "position");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 7, "normals");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 8, "materials");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 9, "emission");
			s_Data->m_DebugPipeline.UpdateSampler(&s_Data->m_GFramebuffer, 10, "shadow_coord");
		}


		// Bloom
		{
			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			DynamicPipelineCI.eCullMode = CullMode::None;
			DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_PostProcessingFramebuffer;

			GraphicsPipelineShaderCreateInfo shaderCI = {};
			shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
			shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Bloom.frag";
			DynamicPipelineCI.ShaderCreateInfo = shaderCI;
			DynamicPipelineCI.PipelineName = "Bloom";

			auto result = s_Data->m_BloomPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
			s_Data->m_BloomPipeline.UpdateSampler(&s_Data->m_HDRFramebuffer, 0);
		}

		// Blur
		{
			GraphicsPipelineCreateInfo DynamicPipelineCI = {};
			DynamicPipelineCI.eCullMode = CullMode::None;
			DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_PostProcessingFramebuffer;

			GraphicsPipelineShaderCreateInfo shaderCI = {};
			shaderCI.FilePaths[ShaderType::Vertex] = s_Data->m_Path + "Shaders/GenTriangle.vert";
			shaderCI.FilePaths[ShaderType::Fragment] = s_Data->m_Path + "Shaders/Blur.frag";
			DynamicPipelineCI.ShaderCreateInfo = shaderCI;
			DynamicPipelineCI.PipelineName = "Blur";
			DynamicPipelineCI.pTargetFramebuffer = &s_Data->m_PostProcessingFramebuffer;

			auto result = s_Data->m_BlurPipeline.Create(&DynamicPipelineCI);
			assert(result == PipelineCreateResult::SUCCESS);
			s_Data->m_BlurPipeline.UpdateSampler(&s_Data->m_HDRFramebuffer, 0);
		}
		
#endif

		
	}

	void DeferredRenderer::InitFramebuffers()
	{
		// Main
		s_Data->f_Main = GraphicsContext::GetSingleton()->GetFramebuffer();

#ifdef FROSTIUM_SMOLENGINE_IMPL

		JobsSystemInstance::BeginSubmition();
		{
			JobsSystemInstance::Schedule([&]()
			{
				// Gbuffer
				{
					const bool ClearOp = true;
					FramebufferAttachment albedro = FramebufferAttachment(AttachmentFormat::Color, ClearOp, "albedro");
					FramebufferAttachment position = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "position");
					FramebufferAttachment normals = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "normals");
					FramebufferAttachment materials = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "materials");
					FramebufferAttachment emission = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "emission");
					FramebufferAttachment shadow_coord = FramebufferAttachment(AttachmentFormat::SFloat4_32, ClearOp, "shadow_coord");

					FramebufferSpecification framebufferCI = {};
					framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
					framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
					framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
					framebufferCI.Attachments = { albedro, position, normals, materials, emission, shadow_coord};

					Framebuffer::Create(framebufferCI, &s_Data->f_GBuffer);
				}
				
			});

			JobsSystemInstance::Schedule([&]()
			{
				// Lighting
				{
					FramebufferAttachment color_1 = FramebufferAttachment(AttachmentFormat::SFloat4_32, true, "color_1");
					FramebufferAttachment color_2 = FramebufferAttachment(AttachmentFormat::SFloat4_32, true, "color_2");
					FramebufferAttachment color_3 = FramebufferAttachment(AttachmentFormat::SFloat4_16, true, "color_3");

					FramebufferSpecification framebufferCI = {};
					framebufferCI.eSamplerFiltering = SamplerFilter::LINEAR;
					framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
					framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
					framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
					framebufferCI.Attachments = { color_1, color_2, color_3 };

					Framebuffer::Create(framebufferCI, &s_Data->f_Lighting);
				}

			});

			JobsSystemInstance::Schedule([&]()
			{
				// FXAA
				{
					FramebufferSpecification framebufferCI = {};
					framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
					framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
					framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
					framebufferCI.Attachments = { FramebufferAttachment(AttachmentFormat::SFloat4_16, true) };

					Framebuffer::Create(framebufferCI, &s_Data->f_FXAA);
				}

			});

			JobsSystemInstance::Schedule([&]()
			{
				// Depth
				{
					FramebufferSpecification framebufferCI = {};
					uint32_t size = 8192;
					switch (s_Data->m_MapSize)
					{
					case ShadowMapSize::SIZE_2: size = 2048; break;
					case ShadowMapSize::SIZE_4: size = 4096; break;
					case ShadowMapSize::SIZE_8: size = 8192; break;
					case ShadowMapSize::SIZE_16: size = 16384; break;
					}
					framebufferCI.Width = size;
					framebufferCI.Height = size;
					framebufferCI.bResizable = false;
					framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
					framebufferCI.eSpecialisation = FramebufferSpecialisation::ShadowMap;

					Framebuffer::Create(framebufferCI, &s_Data->f_Depth);
				}

			});

			// Bloom
			JobsSystemInstance::Schedule([&]()
			{
					FramebufferAttachment color = FramebufferAttachment(AttachmentFormat::SFloat4_32, true, "color_1");

					FramebufferSpecification framebufferCI = {};
					framebufferCI.eSamplerFiltering = SamplerFilter::LINEAR;
					framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
					framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
					framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
					framebufferCI.Attachments = { color };

					Framebuffer::Create(framebufferCI, &s_Data->f_Bloom);
			});
		}
		JobsSystemInstance::EndSubmition();
#else
        // Gbuffer
		{
			const bool ClearOp = true;
			FramebufferAttachment albedro = FramebufferAttachment(AttachmentFormat::Color, ClearOp, "albedro");
			FramebufferAttachment position = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "position");
			FramebufferAttachment normals = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "normals");
			FramebufferAttachment materials = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "materials");
			FramebufferAttachment emission = FramebufferAttachment(AttachmentFormat::SFloat4_16, ClearOp, "emission");
			FramebufferAttachment shadow_coord = FramebufferAttachment(AttachmentFormat::SFloat4_32, ClearOp, "shadow_coord");

			FramebufferSpecification framebufferCI = {};
			framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
			framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
			framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
			framebufferCI.Attachments = { albedro, position, normals, materials, emission, shadow_coord };

			Framebuffer::Create(framebufferCI, &s_Data->m_GFramebuffer);
		}

		// Lighting
		{
			FramebufferAttachment color_1 = FramebufferAttachment(AttachmentFormat::SFloat4_32, true);

			FramebufferSpecification framebufferCI = {};
			framebufferCI.eSamplerFiltering = SamplerFilter::LINEAR;
			framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
			framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
			framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
			framebufferCI.Attachments = { color_1 };

			Framebuffer::Create(framebufferCI, &s_Data->m_LightingFramebuffer);
		}

		// HDR
		{
			FramebufferAttachment color_1 = FramebufferAttachment(AttachmentFormat::SFloat4_32, true, "color_1");
			FramebufferAttachment color_2 = FramebufferAttachment(AttachmentFormat::SFloat4_32, true, "color_2");

			FramebufferSpecification framebufferCI = {};
			framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
			framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
			framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
			framebufferCI.Attachments = { color_1, color_2 };

			Framebuffer::Create(framebufferCI, &s_Data->m_HDRFramebuffer);
		}

		// Depth
		{
			FramebufferSpecification framebufferCI = {};
			uint32_t size = 8192;
			switch (s_Data->m_MapSize)
			{
			case ShadowMapSize::SIZE_2: size = 2048; break;
			case ShadowMapSize::SIZE_4: size = 4096; break;
			case ShadowMapSize::SIZE_8: size = 8192; break;
			case ShadowMapSize::SIZE_16: size = 16384; break;
			}
			framebufferCI.Width = size;
			framebufferCI.Height = size;
			framebufferCI.bResizable = false;
			framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
			framebufferCI.eSpecialisation = FramebufferSpecialisation::ShadowMap;

			Framebuffer::Create(framebufferCI, &s_Data->m_DepthFramebuffer);
		}

		// Post Processing
		{
			FramebufferAttachment bloom = FramebufferAttachment(AttachmentFormat::SFloat4_32, true);

			FramebufferSpecification framebufferCI = {};
			framebufferCI.Width = GraphicsContext::GetSingleton()->GetWindowData()->Width;
			framebufferCI.Height = GraphicsContext::GetSingleton()->GetWindowData()->Height;
			framebufferCI.eMSAASampels = MSAASamples::SAMPLE_COUNT_1;
			framebufferCI.Attachments = { bloom };

			Framebuffer::Create(framebufferCI, &s_Data->m_PostProcessingFramebuffer);
		}
#endif

	}

	void DeferredRenderer::CalculateDepthMVP(glm::mat4& out_mvp)
	{
		// Keep depth range as small as possible
		// for better shadow map precision
		auto& data = s_Data->m_DirLight;

		// Matrix from light's point of view
		glm::mat4 depthProjectionMatrix = glm::perspective(data.lightFOV, 1.0f, data.zNear, data.zFar);
		glm::mat4 depthViewMatrix = glm::lookAt(glm::vec3(data.Direction), glm::vec3(0.0f), glm::vec3(0, 1, 0));
		glm::mat4 depthModelMatrix = glm::mat4(1.0f);

		out_mvp =  depthProjectionMatrix * depthViewMatrix * depthModelMatrix;
	}

	void DeferredRenderer::AddMesh(const glm::vec3& pos, const glm::vec3& rotation, const glm::vec3& scale, Mesh* mesh, const uint32_t& materialID)
	{
		if (s_Data->m_Objects >= max_objects)
			StartNewBacth();

		auto& instance = s_Data->m_Packages[mesh];
		InstancePackage::Package* package = nullptr;

		if (instance.CurrentIndex == instance.Packages.size())
		{
			instance.Packages.emplace_back(InstancePackage::Package());
			package = &instance.Packages.back();
		}
		else
			package = &instance.Packages[instance.CurrentIndex];

		package->MaterialID = materialID;
		package->WorldPos = const_cast<glm::vec3*>(&pos);
		package->Rotation = const_cast<glm::vec3*>(&rotation);
		package->Scale = const_cast<glm::vec3*>(&scale);

		instance.CurrentIndex++;
		s_Data->m_Objects++;

		bool found = std::find(s_Data->m_UsedMeshes.begin(), s_Data->m_UsedMeshes.end(), mesh) != s_Data->m_UsedMeshes.end();
		if (found == false)
			s_Data->m_UsedMeshes.emplace_back(mesh);

		for (auto& sub : mesh->m_Childs)
			AddMesh(pos, rotation, scale, &sub, sub.m_MaterialID);
	}

	bool DeferredRenderer::UpdateMaterials()
	{
		if (!s_Data->m_IsInitialized)
			return false;

		std::vector<Texture*> tetxures;
		MaterialLibrary::GetSinglenton()->GetTextures(tetxures);
		s_Data->p_Gbuffer.UpdateSamplers(tetxures, s_Data->m_TexturesBinding);

		void* data = nullptr;
		uint32_t size = 0;
		MaterialLibrary::GetSinglenton()->GetMaterialsPtr(data, size);
		s_Data->p_Gbuffer.SubmitBuffer(s_Data->m_MaterialsBinding, size, data);
		return true;
	}

	bool DeferredRenderer::ResetStates()
	{
		s_Data->m_DirLight = {};
		s_Data->m_State = {};

		Reset();
		return true;
	}

	void DeferredRenderer::OnResize(uint32_t width, uint32_t height)
	{
		s_Data->f_GBuffer.OnResize(width, height);
		{
			// Lighting pipeline
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 5, "albedro");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 6, "position");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 7, "normals");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 8, "materials");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 9, "emission");
			s_Data->p_Lighting.UpdateSampler(&s_Data->f_GBuffer, 10, "shadow_coord");

			// Debug view pipeline
			s_Data->p_Debug.UpdateSampler(&s_Data->f_Depth, 1, "Depth_Attachment");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 5, "albedro");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 6, "position");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 7, "normals");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 8, "materials");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 9, "emission");
			s_Data->p_Debug.UpdateSampler(&s_Data->f_GBuffer, 10, "shadow_coord");
		}

		s_Data->f_Lighting.OnResize(width, height);
		{
			s_Data->p_FXAA.UpdateSampler(&s_Data->f_Lighting, 0);
		}

		s_Data->f_FXAA.OnResize(width, height);
		{
			s_Data->p_Combination.UpdateSampler(&s_Data->f_FXAA, 0);
		}

		s_Data->f_Bloom.OnResize(width, height);
		s_Data->p_Bloom.UpdateSampler(&s_Data->f_Lighting, 0, "color_2");
		s_Data->p_Bloom.UpdateSampler(&s_Data->f_Lighting, 1, "color_3");
		{
			s_Data->p_Combination.UpdateSampler(&s_Data->f_Bloom, 1);
		}
	}

	void DeferredRenderer::Reset()
	{
		s_Data->m_Objects = 0;
		s_Data->m_InstanceDataIndex = 0;
		s_Data->m_PointLightIndex = 0;
		s_Data->m_SpotLightIndex = 0;
		s_Data->m_LastAnimationOffset = 0;
		s_Data->m_UsedMeshes.clear();
		s_Data->m_RootOffsets.clear();
	}

	void DeferredRenderer::Shutdown()
	{
		if (s_Data != nullptr)
			s_Data->m_IsInitialized = false;
	}

	Framebuffer* DeferredRenderer::GetFramebuffer()
	{
		return s_Data->f_Main;
	}

	uint32_t DeferredRenderer::GetNumObjects()
	{
		return s_Data->m_Objects;
	}

}