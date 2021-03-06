//  -------------------------------------------------------------------------
//  Copyright (C) 2014 BMW Car IT GmbH
//  -------------------------------------------------------------------------
//  This Source Code Form is subject to the terms of the Mozilla Public
//  License, v. 2.0. If a copy of the MPL was not distributed with this
//  file, You can obtain one at https://mozilla.org/MPL/2.0/.
//  -------------------------------------------------------------------------

#include "ramses-client-api/Scene.h"
#include "ramses-client-api/Resource.h"
#include "ramses-client-api/EffectDescription.h"
#include "ramses-sdk-build-config.h"

// internal
#include "SceneImpl.h"
#include "SceneConfigImpl.h"
#include "ResourceImpl.h"
#include "AnimationSystemImpl.h"
#include "AnimatedPropertyImpl.h"
#include "RamsesFrameworkImpl.h"
#include "RamsesClientImpl.h"
#include "RamsesObjectRegistryIterator.h"
#include "SerializationHelper.h"
#include "RamsesVersion.h"
#include "SceneReferenceImpl.h"

// framework
#include "SceneAPI/SceneCreationInformation.h"
#include "Scene/ScenePersistation.h"
#include "Scene/ClientScene.h"
#include "Components/ResourcePersistation.h"
#include "Components/ManagedResource.h"
#include "Components/ResourceTableOfContents.h"
#include "Animation/AnimationSystemFactory.h"
#include "Resource/IResource.h"
#include "ClientCommands/PrintSceneList.h"
#include "ClientCommands/ForceFallbackImage.h"
#include "ClientCommands/FlushSceneVersion.h"
#include "SerializationContext.h"
#include "Utils/BinaryFileOutputStream.h"
#include "Utils/BinaryFileInputStream.h"
#include "Utils/LogContext.h"
#include "Utils/File.h"
#include "Collections/IInputStream.h"
#include "Collections/String.h"
#include "Collections/HashMap.h"
#include "PlatformAbstraction/PlatformTime.h"
#include "Utils/LogMacros.h"
#include "Utils/RamsesLogger.h"
#include "ClientFactory.h"
#include "FrameworkFactoryRegistry.h"
#include "Ramsh/Ramsh.h"
#include "DataTypeUtils.h"
#include "ResourceDataPoolImpl.h"
#include "Resource/ArrayResource.h"
#include "Resource/EffectResource.h"
#include "Resource/TextureResource.h"
#include "glslEffectBlock/GlslEffect.h"
#include "EffectDescriptionImpl.h"
#include "TextureUtils.h"

#include "PlatformAbstraction/PlatformTypes.h"
#include <array>

namespace ramses
{
    static const bool clientRegisterSuccess = ClientFactory::RegisterClientFactory();

    RamsesClientImpl::RamsesClientImpl(RamsesFrameworkImpl& framework,  const char* applicationName)
        : RamsesObjectImpl(ERamsesObjectType_Client, applicationName)
        , m_appLogic(framework.getParticipantAddress().getParticipantId(), framework.getFrameworkLock())
        , m_sceneFactory()
        , m_framework(framework)
        , m_loadFromFileTaskQueue(framework.getTaskQueue())
        , m_deleteSceneQueue(framework.getTaskQueue())
        , m_clientResourceCacheTimeout(5000)
        , m_resourceDataPool(*new ResourceDataPoolImpl(*this))
    {
        assert(!framework.isConnected());

        m_appLogic.init(framework.getResourceComponent(), framework.getScenegraphComponent());
        m_cmdPrintSceneList.reset(new ramses_internal::PrintSceneList(*this));
        m_cmdPrintValidation.reset(new ramses_internal::ValidateCommand(*this));
        m_cmdForceFallbackImage.reset(new ramses_internal::ForceFallbackImage(*this));
        m_cmdFlushSceneVersion.reset(new ramses_internal::FlushSceneVersion(*this));
        m_cmdDumpSceneToFile.reset(new ramses_internal::DumpSceneToFile(*this));
        m_cmdLogResourceMemoryUsage.reset(new ramses_internal::LogResourceMemoryUsage(*this));
        framework.getRamsh().add(*m_cmdPrintSceneList);
        framework.getRamsh().add(*m_cmdPrintValidation);
        framework.getRamsh().add(*m_cmdForceFallbackImage);
        framework.getRamsh().add(*m_cmdFlushSceneVersion);
        framework.getRamsh().add(*m_cmdDumpSceneToFile);
        framework.getRamsh().add(*m_cmdLogResourceMemoryUsage);
        m_framework.getPeriodicLogger().registerPeriodicLogSupplier(&m_framework.getScenegraphComponent());
    }

    RamsesClientImpl::~RamsesClientImpl()
    {
        m_deleteSceneQueue.disableAcceptingTasksAfterExecutingCurrentQueue();
        m_loadFromFileTaskQueue.disableAcceptingTasksAfterExecutingCurrentQueue();

        // delete async loaded  scenes that were never collected via calling dispatchEvents
        ramses_internal::PlatformGuard g(m_clientLock);
        for (auto& loadStatus : m_asyncSceneLoadStatusVec)
        {
            delete loadStatus.scene;
        }

        for (auto& scene : m_scenes)
        {
            delete scene;
        }

        m_framework.getPeriodicLogger().removePeriodicLogSupplier(&m_framework.getScenegraphComponent());
    }

    void RamsesClientImpl::setHLObject(RamsesClient* hlClient)
    {
        assert(hlClient);
        m_hlClient = hlClient;
    }


    void RamsesClientImpl::deinitializeFrameworkData()
    {
    }

    const ramses_internal::ClientApplicationLogic& RamsesClientImpl::getClientApplication() const
    {
        return m_appLogic;
    }

    ramses_internal::ClientApplicationLogic& RamsesClientImpl::getClientApplication()
    {
        return m_appLogic;
    }

    Scene* RamsesClientImpl::createScene(sceneId_t sceneId, const SceneConfigImpl& sceneConfig, const char* name)
    {
        if (!sceneId.isValid())
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::createScene: invalid sceneId");
            return nullptr;
        }

        ramses_internal::PlatformGuard g(m_clientLock);
        ramses_internal::SceneInfo sceneInfo;
        sceneInfo.friendlyName = name;
        sceneInfo.sceneID = ramses_internal::SceneId(sceneId.getValue());

        ramses_internal::ClientScene* internalScene = m_sceneFactory.createScene(sceneInfo);
        if (nullptr == internalScene)
        {
            return nullptr;
        }

        SceneImpl& pimpl = *new SceneImpl(*internalScene, sceneConfig, *m_hlClient);
        pimpl.initializeFrameworkData();
        Scene* scene = new Scene(pimpl);
        m_scenes.push_back(scene);

        return scene;
    }

    RamsesClientImpl::DeleteSceneRunnable::DeleteSceneRunnable(Scene* scene, ramses_internal::ClientScene* llscene)
        : m_scene(scene)
        , m_lowLevelScene(llscene)
    {
    }

    void RamsesClientImpl::DeleteSceneRunnable::execute()
    {
        delete m_scene;
        delete m_lowLevelScene;
    }

    status_t RamsesClientImpl::destroy(Scene& scene)
    {
        ramses_internal::PlatformGuard g(m_clientLock);
        SceneVector::iterator iter = ramses_internal::find_c(m_scenes, &scene);
        if (iter != m_scenes.end())
        {
            m_scenes.erase(iter);

            const ramses_internal::SceneId sceneID(scene.impl.getSceneId().getValue());
            auto llscene = m_sceneFactory.releaseScene(sceneID);

            getClientApplication().removeScene(sceneID);

            auto task = new DeleteSceneRunnable(&scene, llscene);
            m_deleteSceneQueue.enqueue(*task);
            task->release();

            return StatusOK;
        }

        return addErrorEntry("RamsesClient::destroy failed, scene is not in this client.");
    }

    void RamsesClientImpl::writeLowLevelResourcesToStream(const ResourceObjects& resources, ramses_internal::BinaryFileOutputStream& resourceOutputStream, bool compress) const
    {
        //getting names for resources (names are transmitted only for debugging purposes)
        ramses_internal::ManagedResourceVector managedResources;
        managedResources.reserve(resources.size());
        for (const auto res : resources)
        {
            assert(res != nullptr);
            const ramses_internal::ResourceContentHash& hash = res->impl.getLowlevelResourceHash();
            const ramses_internal::ManagedResource managedRes = getClientApplication().getResource(hash);
            if (managedRes)
            {
                managedResources.push_back(managedRes);
            }
            else
            {
                const ramses_internal::ManagedResource forceLoadedResource = getClientApplication().forceLoadResource(hash);
                assert(forceLoadedResource);
                managedResources.push_back(forceLoadedResource);
            }
        }

        // sort resources by hash to maintain a deterministic order in which we write them to file, remove duplicates
        std::sort(managedResources.begin(), managedResources.end(), [](auto const& a, auto const& b) { return a->getHash() < b->getHash(); });
        managedResources.erase(std::unique(managedResources.begin(), managedResources.end()), managedResources.end());

        // write LL-TOC and LL resources
        ramses_internal::ResourcePersistation::WriteNamedResourcesWithTOCToStream(resourceOutputStream, managedResources, compress);
    }

    ramses_internal::ManagedResource RamsesClientImpl::getResource(ramses_internal::ResourceContentHash hash) const
    {
        return m_appLogic.getResource(hash);
    }

    Scene* RamsesClientImpl::prepareSceneFromInputStream(const char* caller, std::string const& filename, ramses_internal::IInputStream& inputStream, bool localOnly)
    {
        LOG_TRACE(ramses_internal::CONTEXT_CLIENT, "RamsesClient::prepareSceneFromInputStream:  start loading scene from input stream");

        ramses_internal::SceneCreationInformation createInfo;
        ramses_internal::ScenePersistation::ReadSceneMetadataFromStream(inputStream, createInfo);
        const ramses_internal::SceneSizeInformation& sizeInformation = createInfo.m_sizeInfo;
        const ramses_internal::SceneInfo sceneInfo(createInfo.m_id, createInfo.m_name);

        LOG_DEBUG(ramses_internal::CONTEXT_CLIENT, "RamsesClient::prepareSceneFromInputStream:  scene to be loaded has " << sizeInformation.asString());

        ramses_internal::ClientScene* internalScene = nullptr;
        {
            ramses_internal::PlatformGuard g(m_clientLock);
            internalScene = m_sceneFactory.createScene(sceneInfo);
        }
        if (nullptr == internalScene)
        {
            return nullptr;
        }
        internalScene->preallocateSceneSize(sizeInformation);

        ramses_internal::AnimationSystemFactory animSystemFactory(ramses_internal::EAnimationSystemOwner_Client, &internalScene->getSceneActionCollection());

        // need first to create the pimpl, so that internal framework components know the new scene
        SceneConfigImpl sceneConfig;
        if (localOnly)
        {
            LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RamsesClient::" << caller << ": Mark file loaded from " << filename << " with sceneId " << createInfo.m_id << " as local only");
            sceneConfig.setPublicationMode(EScenePublicationMode_LocalOnly);
        }

        SceneImpl& pimpl = *new SceneImpl(*internalScene, sceneConfig, *m_hlClient);

        // now the scene is registered, so it's possible to load the low level content into the scene
        LOG_TRACE(ramses_internal::CONTEXT_CLIENT, "    Reading low level scene from stream");
        ramses_internal::ScenePersistation::ReadSceneFromStream(inputStream, *internalScene, &animSystemFactory);

        LOG_TRACE(ramses_internal::CONTEXT_CLIENT, "    Deserializing high level scene objects from stream");
        DeserializationContext deserializationContext;
        SerializationHelper::DeserializeObjectID(inputStream);
        const auto stat = pimpl.deserialize(inputStream, deserializationContext);
        if (stat != StatusOK)
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "    Failed to deserialize high level scene:");
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, getStatusMessage(stat));
            delete &pimpl;
            return nullptr;
        }

        LOG_TRACE(ramses_internal::CONTEXT_CLIENT, "    Done with preparing scene from input stream.");

        return new Scene(pimpl);
    }

    Scene* RamsesClientImpl::prepareSceneFromFile(const char* caller, std::string const& sceneFilename, bool localOnly)
    {
        // this file contains scene data AND resource data and will be handed over to and held open by resource component as resource stream
        ramses_internal::ResourceFileInputStreamSPtr sceneAndResourceFileStream(new ramses_internal::ResourceFileInputStream(sceneFilename.c_str()));
        ramses_internal::BinaryFileInputStream& inputStream = sceneAndResourceFileStream->resourceStream;
        if (inputStream.getState() != ramses_internal::EStatus::Ok)
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::" << caller << ":  failed to open file");
            return nullptr;
        }

        if (!ReadRamsesVersionAndPrintWarningOnMismatch(inputStream, "scene file"))
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::" << caller << ": failed to read from file");
            return nullptr;
        }

        uint64_t sceneObjectStart = 0;
        uint64_t llResourceStart = 0;
        inputStream >> sceneObjectStart;
        inputStream >> llResourceStart;

        Scene* scene = prepareSceneFromInputStream(caller, sceneFilename, inputStream, localOnly);

        // calls on m_appLogic are thread safe
        if (!m_appLogic.hasResourceFile(sceneFilename.c_str()))
        {
            // register resource file for on-demand loading (LL-Resources)
            ramses_internal::ResourceTableOfContents loadedTOC;
            loadedTOC.readTOCPosAndTOCFromStream(inputStream);
            m_appLogic.addResourceFile(sceneAndResourceFileStream, loadedTOC);
            scene->impl.setSceneFileName(sceneFilename);
        }

        return scene;
    }

    Scene* RamsesClientImpl::loadSceneFromFile(const char* fileName, bool localOnly)
    {
        const ramses_internal::UInt64 start = ramses_internal::PlatformTime::GetMillisecondsMonotonic();
        Scene* scene = prepareSceneFromFile("loadSceneFromFile", fileName ? fileName : "", localOnly);
        if (!scene)
        {
            return nullptr;
        }
        finalizeLoadedScene(scene);

        const ramses_internal::UInt64 end = ramses_internal::PlatformTime::GetMillisecondsMonotonic();
        LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RamsesClient::loadSceneFromFile  Scene loaded from '" << fileName << "' in " << (end - start) << " ms");

        return scene;
    }

    void RamsesClientImpl::finalizeLoadedScene(Scene* scene)
    {
        // add to the known list of scenes
        ramses_internal::PlatformGuard g(m_clientLock);
        m_scenes.push_back(scene);
    }

    void RamsesClientImpl::WriteCurrentBuildVersionToStream(ramses_internal::IOutputStream& stream)
    {
        ramses_internal::RamsesVersion::WriteToStream(stream, ::ramses_sdk::RAMSES_SDK_PROJECT_VERSION_STRING, ::ramses_sdk::RAMSES_SDK_GIT_COMMIT_HASH);
    }

    bool RamsesClientImpl::ReadRamsesVersionAndPrintWarningOnMismatch(ramses_internal::BinaryFileInputStream& inputStream, const ramses_internal::String& verboseFileName)
    {
        // return false on read error only, not version mismatch
        ramses_internal::RamsesVersion::VersionInfo readVersion;
        if (!ramses_internal::RamsesVersion::ReadFromStream(inputStream, readVersion))
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::ReadRamsesVersionAndPrintWarningOnMismatch: failed to read RAMSES version for " << verboseFileName << ", file probably corrupt. Loading aborted.");
            return false;
        }
        LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RAMSES version in file '" << verboseFileName << "': [" << readVersion.versionString << "]; GitHash: [" << readVersion.gitHash << "]");

        if (!ramses_internal::RamsesVersion::MatchesMajorMinor(::ramses_sdk::RAMSES_SDK_PROJECT_VERSION_MAJOR_INT, ::ramses_sdk::RAMSES_SDK_PROJECT_VERSION_MINOR_INT, readVersion))
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::ReadRamsesVersionAndPrintWarningOnMismatch: Version of file " << verboseFileName << "does not match MAJOR.MINOR of this build. Cannot load the file.");
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "SDK version of loader: [" << ::ramses_sdk::RAMSES_SDK_PROJECT_VERSION_STRING << "]; GitHash: [" << ::ramses_sdk::RAMSES_SDK_GIT_COMMIT_HASH << "]");
            return false;
        }
        return true;
    }

    status_t RamsesClientImpl::loadSceneFromFileAsync(const char* fileName, bool localOnly)
    {
        LoadSceneRunnable* task = new LoadSceneRunnable(*this, fileName, localOnly);
        m_loadFromFileTaskQueue.enqueue(*task);
        task->release();
        return StatusOK;
    }

    SceneReference* RamsesClientImpl::findSceneReference(sceneId_t masterSceneId, sceneId_t referencedSceneId)
    {
        for (auto const& scene : getListOfScenes())
        {
            if (masterSceneId == scene->getSceneId())
                return scene->impl.getSceneReference(referencedSceneId);
        }

        return nullptr;
    }

    status_t RamsesClientImpl::dispatchEvents(IClientEventHandler& clientEventHandler)
    {
        std::vector<SceneLoadStatus> localAsyncSceneLoadStatus;
        {
            ramses_internal::PlatformGuard g(m_clientLock);
            localAsyncSceneLoadStatus.swap(m_asyncSceneLoadStatusVec);
        }

        for (const auto& sceneStatus : localAsyncSceneLoadStatus)
        {
            if (sceneStatus.scene)
            {
                // finalize scene
                Scene* scene = sceneStatus.scene;
                const ramses_internal::UInt64 start = ramses_internal::PlatformTime::GetMillisecondsMonotonic();
                finalizeLoadedScene(scene);
                const ramses_internal::UInt64 end = ramses_internal::PlatformTime::GetMillisecondsMonotonic();
                LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RamsesClient::dispatchEvents(sceneFileLoadSucceeded): Synchronous postprocessing of scene loaded from '" <<
                         sceneStatus.sceneFilename << "' (sceneName: " << scene->getName() << ", sceneId " << scene->getSceneId() << ") in " << (end - start) << " ms");

                clientEventHandler.sceneFileLoadSucceeded(sceneStatus.sceneFilename.c_str(), scene);
            }
            else
            {
                LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RamsesClient::dispatchEvents(sceneFileLoadFailed): " << sceneStatus.sceneFilename);
                clientEventHandler.sceneFileLoadFailed(sceneStatus.sceneFilename.c_str());
            }
        }

        const auto clientRendererEvents = getClientApplication().popSceneReferenceEvents();
        for (const auto& rendererEvent : clientRendererEvents)
        {
            switch (rendererEvent.type)
            {
            case ramses_internal::SceneReferenceEventType::SceneStateChanged:
            {
                auto sr = findSceneReference(sceneId_t{ rendererEvent.masterSceneId.getValue() }, sceneId_t{ rendererEvent.referencedScene.getValue() });
                if (sr)
                {
                    sr->impl.setReportedState(SceneReferenceImpl::GetSceneReferenceState(rendererEvent.sceneState));
                    clientEventHandler.sceneReferenceStateChanged(*sr, SceneReferenceImpl::GetSceneReferenceState(rendererEvent.sceneState));
                }
                else
                    LOG_WARN(CONTEXT_CLIENT, "RamsesClientImpl::dispatchEvents: did not find SceneReference for a SceneStateChanged event: "
                        << rendererEvent.masterSceneId << " " << rendererEvent.referencedScene << " " << EnumToString(rendererEvent.sceneState));
                break;
            }
            case ramses_internal::SceneReferenceEventType::SceneFlushed:
            {
                auto sr = findSceneReference(sceneId_t{ rendererEvent.masterSceneId.getValue() }, sceneId_t{ rendererEvent.referencedScene.getValue() });
                if (sr)
                    clientEventHandler.sceneReferenceFlushed(*sr, sceneVersionTag_t{ rendererEvent.tag.getValue() });
                else
                    LOG_WARN(CONTEXT_CLIENT, "RamsesClientImpl::dispatchEvents: did not find SceneReference for a SceneFlushed event: "
                        << rendererEvent.masterSceneId << " " << rendererEvent.referencedScene << " " << rendererEvent.tag);
                break;
            }
            case ramses_internal::SceneReferenceEventType::DataLinked:
                clientEventHandler.dataLinked(sceneId_t{ rendererEvent.providerScene.getValue() }, dataProviderId_t{ rendererEvent.dataProvider.getValue() },
                    sceneId_t{ rendererEvent.consumerScene.getValue() }, dataConsumerId_t{ rendererEvent.dataConsumer.getValue() }, rendererEvent.status);
                break;
            case ramses_internal::SceneReferenceEventType::DataUnlinked:
                clientEventHandler.dataUnlinked(sceneId_t{ rendererEvent.consumerScene.getValue() }, dataConsumerId_t{ rendererEvent.dataConsumer.getValue() }, rendererEvent.status);
                break;
            }
        }

        return StatusOK;
    }

    RamsesClientImpl::LoadSceneRunnable::LoadSceneRunnable(RamsesClientImpl& client, std::string const& sceneFilename, bool localOnly)
        : m_client(client)
        , m_sceneFilename(sceneFilename)
        , m_localOnly(localOnly)
    {
    }

    void RamsesClientImpl::LoadSceneRunnable::execute()
    {
        const ramses_internal::UInt64 start = ramses_internal::PlatformTime::GetMillisecondsMonotonic();
        Scene* scene = m_client.prepareSceneFromFile("loadSceneFromFileAsync", m_sceneFilename, m_localOnly);
        const ramses_internal::UInt64 end = ramses_internal::PlatformTime::GetMillisecondsMonotonic();

        if (scene)
        {
            LOG_INFO(ramses_internal::CONTEXT_CLIENT, "RamsesClient::loadSceneFromFileAsync: Scene loaded from '" << m_sceneFilename <<
                     "' (sceneName: " << scene->getName() << ", sceneId " << scene->getSceneId() << ") in " << (end - start) << " ms");
        }

        ramses_internal::PlatformGuard g(m_client.m_clientLock);
        m_client.m_asyncSceneLoadStatusVec.push_back({scene, m_sceneFilename});
    }

    SceneVector RamsesClientImpl::getListOfScenes() const
    {
        ramses_internal::PlatformGuard g(m_clientLock);
        return m_scenes;
    }

    ramses_internal::ResourceHashUsage RamsesClientImpl::getHashUsage_ThreadSafe(const ramses_internal::ResourceContentHash& hash) const
    {
        return m_appLogic.getHashUsage(hash);
    }

    ramses_internal::ManagedResource RamsesClientImpl::getResource_ThreadSafe(ramses_internal::ResourceContentHash hash) const
    {
        return m_appLogic.getResource(hash);
    }

    ramses_internal::ManagedResource RamsesClientImpl::forceLoadResource_ThreadSafe(const ramses_internal::ResourceContentHash& hash) const
    {
        return m_appLogic.forceLoadResource(hash);
    }

    const Scene* RamsesClientImpl::findSceneByName(const char* name) const
    {
        ramses_internal::PlatformGuard g(m_clientLock);
        for (const auto& scene : m_scenes)
            if (scene->impl.getName() == name)
                return scene;

        return nullptr;
    }

    Scene* RamsesClientImpl::findSceneByName(const char* name)
    {
        // Non-const version of findObjectByName cast to its const version to avoid duplicating code
        return const_cast<Scene*>((const_cast<const RamsesClientImpl&>(*this)).findSceneByName(name));
    }

    const Scene* RamsesClientImpl::getScene(sceneId_t sceneId) const
    {
        ramses_internal::PlatformGuard g(m_clientLock);
        for (const auto& scene : m_scenes)
            if (scene->getSceneId() == sceneId)
                return scene;

        return nullptr;
    }

    Scene* RamsesClientImpl::getScene(sceneId_t sceneId)
    {
        // Non-const version of findObjectByName cast to its const version to avoid duplicating code
        return const_cast<Scene*>((const_cast<const RamsesClientImpl&>(*this)).getScene(sceneId));
    }


    ramses_internal::ManagedResource RamsesClientImpl::manageResource(const ramses_internal::IResource* res)
    {
        ramses_internal::ManagedResource managedRes = m_appLogic.addResource(res);
        _LOG_HL_CLIENT_API_STR("Created resource with internal hash " << managedRes->getHash() << ", name: " << managedRes->getName());

        return managedRes;
    }

    RamsesClientImpl& RamsesClientImpl::createImpl(const char* name, RamsesFrameworkImpl& components)
    {
        return *new RamsesClientImpl(components, name);
    }

    RamsesFrameworkImpl& RamsesClientImpl::getFramework()
    {
        return m_framework;
    }

    status_t RamsesClientImpl::validate(uint32_t indent, StatusObjectSet& visitedObjects) const
    {
        status_t status = RamsesObjectImpl::validate(indent, visitedObjects);
        indent += IndentationStep;

        const status_t scenesStatus = validateScenes(indent, visitedObjects);
        if (StatusOK != scenesStatus)
        {
            status = scenesStatus;
        }

        return status;
    }

    status_t RamsesClientImpl::validateScenes(uint32_t indent, StatusObjectSet& visitedObjects) const
    {
        ramses_internal::PlatformGuard g(m_clientLock);

        status_t status = StatusOK;
        for(const auto& scene : m_scenes)
        {
            const status_t sceneStatus = addValidationOfDependentObject(indent, scene->impl, visitedObjects);
            if (StatusOK != sceneStatus)
            {
                status = sceneStatus;
            }
        }

        ramses_internal::StringOutputStream msg;
        msg << "Contains " << m_scenes.size() << " scenes";
        addValidationMessage(EValidationSeverity_Info, indent, msg.c_str());

        return status;
    }

    void RamsesClientImpl::onResourceDestroyed(Resource& resource)
    {
        if (m_clientResourceCacheTimeout > std::chrono::milliseconds{ 0u })
        {
            const auto timeNow = std::chrono::steady_clock::now();
            m_clientResourceCache.push_back(std::make_pair(timeNow, m_framework.getResourceComponent().getResourceHashUsage(resource.impl.getLowlevelResourceHash())));
        }
    }

    void RamsesClientImpl::updateClientResourceCache()
    {
        const auto timeNow = std::chrono::steady_clock::now();
        while (m_clientResourceCache.size() != 0 && timeNow > m_clientResourceCache.front().first + m_clientResourceCacheTimeout)
        {
            m_clientResourceCache.pop_front();
        }
    }

    void RamsesClientImpl::setClientResourceCacheTimeout(std::chrono::milliseconds timeout)
    {
        m_clientResourceCacheTimeout = timeout;
    }

    ResourceDataPool& RamsesClientImpl::getResourceDataPool()
    {
        return m_resourceDataPool;
    }

    ResourceDataPool const& RamsesClientImpl::getResourceDataPool() const
    {
        return m_resourceDataPool;
    }

    ramses_internal::ManagedResource RamsesClientImpl::createManagedArrayResource(uint32_t numElements, EDataType type, const void* arrayData, resourceCacheFlag_t cacheFlag, const char* name)
    {
        if (0u == numElements || nullptr == arrayData)
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClientImpl::createManagedArrayResource Array resource must have element count > 0 and data must not be nullptr!");
            return {};
        }

        ramses_internal::EDataType elementType = DataTypeUtils::ConvertDataTypeToInternal(type);
        ramses_internal::EResourceType resourceType = DataTypeUtils::DeductResourceTypeFromDataType(type);

        auto resource = new ramses_internal::ArrayResource(resourceType, numElements, elementType, arrayData, ramses_internal::ResourceCacheFlag(cacheFlag.getValue()), name);
        return manageResource(resource);
    }

    template <typename MipDataStorageType>
    ramses_internal::ManagedResource RamsesClientImpl::createManagedTexture(ramses_internal::EResourceType textureType, uint32_t width, uint32_t height, uint32_t depth, ETextureFormat format, uint32_t mipMapCount, const MipDataStorageType mipLevelData[], bool generateMipChain, const TextureSwizzle& swizzle, resourceCacheFlag_t cacheFlag, const char* name)
    {
        if (!TextureUtils::TextureParametersValid(width, height, depth, mipMapCount) || !TextureUtils::MipDataValid(width, height, depth, mipMapCount, mipLevelData, format))
        {
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::createTexture: invalid parameters");
            return {};
        }

        if (generateMipChain && (!FormatSupportsMipChainGeneration(format) || (mipMapCount > 1)))
        {
            LOG_WARN(ramses_internal::CONTEXT_CLIENT, "RamsesClient::createTexture: cannot auto generate mipmaps when custom mipmap data provided or unsupported format used");
            generateMipChain = false;
        }

        ramses_internal::TextureMetaInfo texDesc;
        texDesc.m_width = width;
        texDesc.m_height = height;
        texDesc.m_depth = depth;
        texDesc.m_format = TextureUtils::GetTextureFormatInternal(format);
        texDesc.m_generateMipChain = generateMipChain;
        texDesc.m_swizzle = TextureUtils::GetTextureSwizzleInternal(swizzle);
        TextureUtils::FillMipDataSizes(texDesc.m_dataSizes, mipMapCount, mipLevelData);

        ramses_internal::TextureResource* resource = new ramses_internal::TextureResource(textureType, texDesc, ramses_internal::ResourceCacheFlag(cacheFlag.getValue()), name);
        TextureUtils::FillMipData(const_cast<ramses_internal::Byte*>(resource->getResourceData().data()), mipMapCount, mipLevelData);

        return manageResource(resource);
    }
    template ramses_internal::ManagedResource RamsesClientImpl::createManagedTexture(ramses_internal::EResourceType textureType, uint32_t width, uint32_t height, uint32_t depth, ETextureFormat format, uint32_t mipMapCount, const MipLevelData mipLevelData[], bool generateMipChain, const TextureSwizzle& swizzle, resourceCacheFlag_t cacheFlag, const char* name);
    template ramses_internal::ManagedResource RamsesClientImpl::createManagedTexture(ramses_internal::EResourceType textureType, uint32_t width, uint32_t height, uint32_t depth, ETextureFormat format, uint32_t mipMapCount, const CubeMipLevelData mipLevelData[], bool generateMipChain, const TextureSwizzle& swizzle, resourceCacheFlag_t cacheFlag, const char* name);

    ramses_internal::ManagedResource RamsesClientImpl::createManagedEffect(const EffectDescription& effectDesc, resourceCacheFlag_t cacheFlag, const char* name, std::string& errorMessages)
    {
        //create effect using vertex and fragment shaders
        ramses_internal::String effectName(name);
        ramses_internal::GlslEffect effectBlock(effectDesc.getVertexShader(), effectDesc.getFragmentShader(), effectDesc.getGeometryShader(), effectDesc.impl.getCompilerDefines(),
            effectDesc.impl.getSemanticsMap(), effectName);
        errorMessages.clear();
        ramses_internal::EffectResource* effectResource = effectBlock.createEffectResource(ramses_internal::ResourceCacheFlag(cacheFlag.getValue()));
        if (!effectResource)
        {
            errorMessages = effectBlock.getEffectErrorMessages().stdRef();
            LOG_ERROR(ramses_internal::CONTEXT_CLIENT, "RamsesClient::createEffect  Failed to create effect resource (name: '" << effectName << "') :\n    " << effectBlock.getEffectErrorMessages());
            return {};
        }
        return manageResource(effectResource);
    }
}
