//  -------------------------------------------------------------------------
//  Copyright (C) 2015 BMW Car IT GmbH
//  -------------------------------------------------------------------------
//  This Source Code Form is subject to the terms of the Mozilla Public
//  License, v. 2.0. If a copy of the MPL was not distributed with this
//  file, You can obtain one at https://mozilla.org/MPL/2.0/.
//  -------------------------------------------------------------------------

#include "Components/ClientSceneLogicBase.h"
#include "Components/ISceneGraphSender.h"
#include "Scene/ClientScene.h"
#include "Scene/SceneDescriber.h"
#include "Scene/SceneActionApplier.h"
#include "Scene/SceneActionCollectionCreator.h"
#include "Scene/SceneResourceUtils.h"
#include "PlatformAbstraction/PlatformTime.h"
#include "Utils/LogMacros.h"
#include "Utils/StatisticCollection.h"
#include "Components/IResourceProviderComponent.h"
#include "Components/SceneUpdate.h"

namespace ramses_internal
{
    ClientSceneLogicBase::ClientSceneLogicBase(ISceneGraphSender& scenegraphProviderComponent, ClientScene& scene, IResourceProviderComponent& res, const Guid& clientAddress)
        : m_scenegraphSender(scenegraphProviderComponent)
        , m_resourceComponent(res)
        , m_myID(clientAddress)
        , m_sceneId(scene.getSceneId())
        , m_scene(scene)
        , m_scenePublicationMode(EScenePublicationMode_Unpublished)
        , m_resourceChanges()
        , m_newClientResources()
        , m_animationSystemFactory(ramses_internal::EAnimationSystemOwner_Scenemanager)
    {
    }

    ClientSceneLogicBase::~ClientSceneLogicBase()
    {
        unpublish();
    }

    void ClientSceneLogicBase::publish(EScenePublicationMode publicationMode)
    {
        if (m_scenePublicationMode == EScenePublicationMode_Unpublished)
        {
            m_scenePublicationMode = publicationMode;
            m_scenegraphSender.sendPublishScene(m_sceneId, publicationMode, m_scene.getName());
        }
    }

    void ClientSceneLogicBase::unpublish()
    {
        if (m_scenePublicationMode != EScenePublicationMode_Unpublished)
        {
            m_scenegraphSender.sendUnpublishScene(m_sceneId, m_scenePublicationMode);

            m_scenePublicationMode = EScenePublicationMode_Unpublished;
        }

        // reset to initial state
        m_subscribersActive.clear();
        m_subscribersWaitingForScene.clear();
    }

    bool ClientSceneLogicBase::isPublished() const
    {
        return m_scenePublicationMode != EScenePublicationMode_Unpublished;
    }

    void ClientSceneLogicBase::addSubscriber(const Guid& newSubscriber)
    {
        if (contains_c(m_subscribersActive, newSubscriber) || contains_c(m_subscribersWaitingForScene, newSubscriber))
        {
            LOG_WARN(CONTEXT_CLIENT, "ClientSceneLogic::addSubscriber: already has " << newSubscriber << " for scene " << m_sceneId);
            return;
        }

        LOG_INFO(CONTEXT_CLIENT, "ClientSceneLogic::addSubscriber: add " << newSubscriber << " for scene " << m_sceneId << ", flushCounter " << m_flushCounter);
        m_subscribersWaitingForScene.push_back(newSubscriber);
        postAddSubscriber();
    }

    void ClientSceneLogicBase::removeSubscriber(const Guid& subscriber)
    {
        auto it = find_c(m_subscribersActive, subscriber);
        if (it != m_subscribersActive.end())
        {
            m_subscribersActive.erase(it);
            LOG_INFO(CONTEXT_CLIENT, "ClientSceneLogic::removeSubscriber: remove active subscriber " << subscriber << " from scene " << m_sceneId << ", numRemaining " << m_subscribersActive.size());
        }
        else
        {
            auto waitingForSceneIter = find_c(m_subscribersWaitingForScene, subscriber);
            if (waitingForSceneIter != m_subscribersWaitingForScene.end())
            {
                m_subscribersWaitingForScene.erase(waitingForSceneIter);
                LOG_INFO(CONTEXT_CLIENT, "ClientSceneLogic::removeSubscriber: remove waiting subscriber " << subscriber << " from scene " << m_sceneId << ", numRemaining " << m_subscribersWaitingForScene.size());
            }
        }
    }

    std::vector<Guid> ClientSceneLogicBase::getWaitingAndActiveSubscribers() const
    {
        std::vector<Guid> result(m_subscribersActive);
        result.insert(result.end(), m_subscribersWaitingForScene.begin(), m_subscribersWaitingForScene.end());
        return result;
    }

    void ClientSceneLogicBase::sendSceneToWaitingSubscribers(const IScene& scene, const FlushTimeInformation& flushTimeInfo, SceneVersionTag versionTag)
    {
        if (m_subscribersWaitingForScene.empty())
        {
            LOG_DEBUG(CONTEXT_CLIENT, "ClientSceneLogicBase::sendSceneToWaitingSubscribers: No subscribers waiting for scene " << m_sceneId);
            return;
        }

        SceneUpdate sceneUpdate;
        SceneActionCollectionCreator creator(sceneUpdate.actions);
        SceneDescriber::describeScene<IScene>(scene, creator);

        m_resourceChanges.clear();
        size_t sceneResourcesSize = 0u;
        SceneResourceUtils::GetAllSceneResourcesFromScene(m_resourceChanges.m_sceneResourceActions, scene, sceneResourcesSize);

        m_resourceChanges.m_addedClientResourceRefs = m_lastFlushClientResourcesInUse;

        // flush asynchronously & check if there already was a named flush
        creator.flush(
            m_flushCounter,
            true,
            scene.getSceneSizeInformation(),
            m_resourceChanges,
            {}, // scene reference actions are transient, not sent to new subscribers
            flushTimeInfo,
            versionTag);
        // TODO vaclav re-enable sending resources after renderer side can use them
        //sceneUpdate.resources = m_resourceComponent.resolveResources(m_resourceChanges.m_addedClientResourceRefs);

        sceneUpdate.flushInfos = { m_flushCounter, versionTag, scene.getSceneSizeInformation(), m_resourceChanges, {}, flushTimeInfo, true, true };
        LOG_INFO(CONTEXT_CLIENT, "Sending scene " << scene.getSceneId() << " to " << m_subscribersWaitingForScene.size() << " subscribers, " <<
            sceneUpdate.actions.numberOfActions() << " scene actions (" << sceneUpdate.actions.collectionData().size() << " bytes)" <<
            m_resourceChanges.m_addedClientResourceRefs.size() << " client resources, " <<
            m_resourceChanges.m_sceneResourceActions.size() << " scene resource actions (" << sceneResourcesSize << " bytes in total used by scene resources)");

        for(const auto& subscriber : m_subscribersWaitingForScene)
        {
            m_scenegraphSender.sendCreateScene(subscriber, m_sceneId, m_scenePublicationMode);
        }
        m_scene.getStatisticCollection().statSceneActionsSent.incCounter(sceneUpdate.actions.numberOfActions()*static_cast<UInt32>(m_subscribersWaitingForScene.size()));
        m_scenegraphSender.sendSceneUpdate(m_subscribersWaitingForScene, std::move(sceneUpdate), m_sceneId, m_scenePublicationMode);

        m_subscribersActive.insert(m_subscribersActive.end(), m_subscribersWaitingForScene.begin(), m_subscribersWaitingForScene.end());
        m_subscribersWaitingForScene.clear();
    }

    void ClientSceneLogicBase::printFlushInfo(StringOutputStream& sos, const char* name, const SceneActionCollection& collection) const
    {
        sos << name << ": SceneID " << m_sceneId << ", flushIdx " << m_flushCounter
            << ", numActions " << collection.numberOfActions() << ", sizeActions " << collection.collectionData().size() << "; ActionCountPerType:\n";

        struct ActionInfo {
            ActionInfo() : count(0), size(0) {}
            UInt32 count;
            UInt32 size;
        };
        std::vector<ActionInfo> sceneActionCountPerType(NumOfSceneActionTypes);
        for (const auto& action : collection)
        {
            ActionInfo& info = sceneActionCountPerType[static_cast<uint32_t>(action.type())];
            ++info.count;
            info.size += action.size();
        }

        for (UInt actionType = 0; actionType < NumOfSceneActionTypes; ++actionType)
        {
            const ActionInfo& info = sceneActionCountPerType[actionType];
            if (info.count > 0)
            {
                sos << name << ": " << GetNameForSceneActionId(static_cast<ESceneActionId>(actionType)) << ": num " << info.count << ", size " << info.size << "\n";
            }
        }
    }

    const char* ClientSceneLogicBase::getSceneStateString() const
    {
        if (m_subscribersActive.size() > 0)
        {
            return "Subscribed";
        }
        else
        {
            switch (m_scenePublicationMode)
            {
            case ramses_internal::EScenePublicationMode_Unpublished:
                return "Unpublished";
            case ramses_internal::EScenePublicationMode_LocalAndRemote:
                return "Published";
            case ramses_internal::EScenePublicationMode_LocalOnly:
                return "Published_LocalOnly";
            default:
                return "Invalid";
            }
        }
    }

    void ClientSceneLogicBase::updateResourceChanges(bool hasNewActions)
    {
        m_resourceChanges.clear();

        // optimization: early out if nothing changed in the scene
        if (!hasNewActions)
            return;

        if (m_scene.getClientResourcesChanged())
        {
            m_newClientResources.clear();
            SceneResourceUtils::GetAllClientResourcesFromScene(m_newClientResources, m_scene);
            SceneResourceUtils::DiffClientResources(m_lastFlushClientResourcesInUse, m_newClientResources, m_resourceChanges);
            m_lastFlushClientResourcesInUse.swap(m_newClientResources);
        }

        m_resourceChanges.m_sceneResourceActions = m_scene.getSceneResourceActions();
    }
}
