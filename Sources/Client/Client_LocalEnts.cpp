/*
 Copyright (c) 2013 yvt
 based on code of pysnip (c) Mathias Kaerlev 2011-2012.
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include "Client.h"
#include <cstdlib>

#include <Core/ConcurrentDispatch.h>
#include <Core/Settings.h>
#include <Core/Strings.h>

#include "IAudioChunk.h"
#include "IAudioDevice.h"

#include "ClientUI.h"
#include "PaletteView.h"
#include "LimboView.h"
#include "MapView.h"
#include "Corpse.h"
#include "ClientPlayer.h"
#include "ILocalEntity.h"
#include "ChatWindow.h"
#include "CenterMessageView.h"
#include "Tracer.h"
#include "FallingBlock.h"
#include "HurtRingView.h"
#include "ParticleSpriteEntity.h"
#include "SmokeSpriteEntity.h"

#include "World.h"
#include "Weapon.h"
#include "GameMap.h"
#include "Grenade.h"

#include "NetClient.h"

SPADES_SETTING(cg_blood, "0");
SPADES_SETTING(cg_reduceSmoke, "0");

namespace spades {
	namespace client {
		
		
#pragma mark - Local Entities / Effects
		
		
		void Client::RemoveAllCorpses(){
			SPADES_MARK_FUNCTION();
			
			corpses.clear();
			lastMyCorpse = nullptr;
		}
		
		
		void Client::RemoveAllLocalEntities(){
			SPADES_MARK_FUNCTION();
			
			localEntities.clear();
		}
		
		void Client::RemoveInvisibleCorpses(){
			SPADES_MARK_FUNCTION();
			
			decltype(corpses)::iterator it;
			std::vector<decltype(it)> its;
			int cnt = (int)corpses.size() - corpseSoftLimit;
			for(it = corpses.begin(); it != corpses.end(); it++){
				if(cnt <= 0)
					break;
				auto& c = *it;
				if(!c->IsVisibleFrom(lastSceneDef.viewOrigin)){
					if(c.get() == lastMyCorpse)
						lastMyCorpse = nullptr;
					its.push_back(it);
				}
				cnt--;
			}
			
			for(size_t i = 0; i < its.size(); i++)
				corpses.erase(its[i]);
			
		}
		
		
		Player *Client::HotTrackedPlayer( hitTag_t* hitFlag ){
			if(!world)
				return nullptr;
			Player *p = world->GetLocalPlayer();
			if(!p || !p->IsAlive())
				return nullptr;
			if(ShouldRenderInThirdPersonView())
				return nullptr;
			Vector3 origin = p->GetEye();
			Vector3 dir = p->GetFront();
			World::WeaponRayCastResult result = world->WeaponRayCast(origin, dir, p);
			
			if(result.hit == false || result.player == nullptr)
				return nullptr;
			
			// don't hot track enemies (non-spectator only)
			if(result.player->GetTeamId() != p->GetTeamId() &&
			   p->GetTeamId() < 2)
				return nullptr;
			if( hitFlag ) {
				*hitFlag = result.hitFlag;
			}
			return result.player;
		}
		
		bool Client::IsMuted() {
			// prevent to play loud sound at connection
			// caused by saved packets
			return time < worldSetTime + .05f;
		}
		
		void Client::Bleed(spades::Vector3 v){
			SPADES_MARK_FUNCTION();
			
			if(!cg_blood)
				return;
			
			// distance cull
			if((v - lastSceneDef.viewOrigin).GetPoweredLength() >
			   150.f * 150.f)
				return;
			
			//Handle<IImage> img = renderer->RegisterImage("Textures/SoftBall.tga");
			Handle<IImage> img = renderer->RegisterImage("Gfx/White.tga");
			Vector4 color = {0.5f, 0.02f, 0.04f, 1.f};
			for(int i = 0; i < 10; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				ent->SetTrajectory(v,
								   MakeVector3(GetRandom()-GetRandom(),
											   GetRandom()-GetRandom(),
											   GetRandom()-GetRandom()) * 10.f,
								   1.f, 0.7f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(0.1f + GetRandom()*GetRandom()*0.2f);
				ent->SetLifeTime(3.f, 0.f, 1.f);
				localEntities.emplace_back(ent);
			}
			
			color = MakeVector4(.7f, .35f, .37f, .6f);
			for(int i = 0; i < 2; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 100.f);
				ent->SetTrajectory(v,
								   MakeVector3(GetRandom()-GetRandom(),
											   GetRandom()-GetRandom(),
											   GetRandom()-GetRandom()) * .7f,
								   .8f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(.5f + GetRandom()*GetRandom()*0.2f,
							   2.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(.20f + GetRandom() * .2f, 0.06f, .20f);
				localEntities.emplace_back(ent);
			}
		}
		
		void Client::EmitBlockFragments(Vector3 origin,
										IntVector3 c){
			SPADES_MARK_FUNCTION();
			
			// distance cull
			float distPowered = (origin - lastSceneDef.viewOrigin).GetPoweredLength();
			if(distPowered >
			   150.f * 150.f)
				return;
			
			Handle<IImage> img = renderer->RegisterImage("Gfx/White.tga");
			Vector4 color = {c.x / 255.f,
				c.y / 255.f, c.z / 255.f, 1.f};
			for(int i = 0; i < 7; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				ent->SetTrajectory(origin,
								   MakeVector3(GetRandom()-GetRandom(),
											   GetRandom()-GetRandom(),
											   GetRandom()-GetRandom()) * 7.f,
								   1.f, .9f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(0.2f + GetRandom()*GetRandom()*0.1f);
				ent->SetLifeTime(2.f, 0.f, 1.f);
				if(distPowered < 16.f * 16.f)
					ent->SetBlockHitAction(ParticleSpriteEntity::BounceWeak);
				localEntities.emplace_back(ent);
			}
			
			if(distPowered <
			   32.f * 32.f){
				for(int i = 0; i < 16; i++){
					ParticleSpriteEntity *ent =
					new ParticleSpriteEntity(this, img, color);
					ent->SetTrajectory(origin,
									   MakeVector3(GetRandom()-GetRandom(),
												   GetRandom()-GetRandom(),
												   GetRandom()-GetRandom()) * 12.f,
									   1.f, .9f);
					ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
					ent->SetRadius(0.1f + GetRandom()*GetRandom()*0.14f);
					ent->SetLifeTime(2.f, 0.f, 1.f);
					if(distPowered < 16.f * 16.f)
						ent->SetBlockHitAction(ParticleSpriteEntity::BounceWeak);
					localEntities.emplace_back(ent);
				}
			}
			
			color += (MakeVector4(1, 1, 1, 1) - color) * .2f;
			color.w *= .2f;
			for(int i = 0; i < 2; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 100.f);
				ent->SetTrajectory(origin,
								   MakeVector3(GetRandom()-GetRandom(),
											   GetRandom()-GetRandom(),
											   GetRandom()-GetRandom()) * .7f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(.6f + GetRandom()*GetRandom()*0.2f,
							   0.8f);
				ent->SetLifeTime(.3f + GetRandom() * .3f, 0.06f, .4f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				localEntities.emplace_back(ent);
			}
			
		}
		
		void Client::EmitBlockDestroyFragments(IntVector3 blk,
											   IntVector3 c){
			SPADES_MARK_FUNCTION();
			
			Vector3 origin = {blk.x + .5f, blk.y + .5f, blk.z + .5f};
			
			// distance cull
			if((origin - lastSceneDef.viewOrigin).GetPoweredLength() >
			   150.f * 150.f)
				return;
			
			Handle<IImage> img = renderer->RegisterImage("Gfx/White.tga");
			Vector4 color = {c.x / 255.f,
				c.y / 255.f, c.z / 255.f, 1.f};
			for(int i = 0; i < 8; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				ent->SetTrajectory(origin,
								   MakeVector3(GetRandom()-GetRandom(),
											   GetRandom()-GetRandom(),
											   GetRandom()-GetRandom()) * 7.f,
								   1.f, 1.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(0.3f + GetRandom()*GetRandom()*0.2f);
				ent->SetLifeTime(2.f, 0.f, 1.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::BounceWeak);
				localEntities.emplace_back(ent);
			}
		}
		
		void Client::MuzzleFire(spades::Vector3 origin,
								spades::Vector3 dir,
								bool local) {
			DynamicLightParam l;
			l.origin = origin;
			l.radius = 5.f;
			l.type = DynamicLightTypePoint;
			l.color = MakeVector3(3.f, 1.6f, 0.5f);
			flashDlights.push_back(l);
			
			Vector4 color;
			Vector3 velBias = {0, 0, -0.5f};
			color = MakeVector4( .8f, .8f, .8f, .3f);
			
			// rapid smoke
			for(int i = 0; i < 2; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 120.f);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												GetRandom()-GetRandom())+velBias*.5f) * 0.3f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(.2f,
							   7.f, 0.0000005f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(0.2f + GetRandom()*0.1f, 0.f, .30f);
				localEntities.emplace_back(ent);
			}
		}
		
		void Client::GrenadeExplosion(spades::Vector3 origin){
			float dist = (origin - lastSceneDef.viewOrigin).GetLength();
			if(dist > 170.f)
				return;
			grenadeVibration += 2.f / (dist + 5.f);
			if(grenadeVibration > 1.f)
				grenadeVibration = 1.f;
			
			DynamicLightParam l;
			l.origin = origin;
			l.radius = 16.f;
			l.type = DynamicLightTypePoint;
			l.color = MakeVector3(3.f, 1.6f, 0.5f);
			l.useLensFlare = true;
			flashDlights.push_back(l);
			
			Vector3 velBias = {0,0,0};
			if(!map->ClipBox(origin.x, origin.y, origin.z)){
				if(map->ClipBox(origin.x + 1.f, origin.y, origin.z)){
					velBias.x -= 1.f;
				}
				if(map->ClipBox(origin.x - 1.f, origin.y, origin.z)){
					velBias.x += 1.f;
				}
				if(map->ClipBox(origin.x, origin.y + 1.f, origin.z)){
					velBias.y -= 1.f;
				}
				if(map->ClipBox(origin.x, origin.y - 1.f, origin.z)){
					velBias.y += 1.f;
				}
				if(map->ClipBox(origin.x, origin.y , origin.z + 1.f)){
					velBias.z -= 1.f;
				}
				if(map->ClipBox(origin.x, origin.y , origin.z - 1.f)){
					velBias.z += 1.f;
				}
			}
			
			Vector4 color;
			color = MakeVector4( .8f, .8f, .8f, .6f);
			// rapid smoke
			for(int i = 0; i < 4; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 60.f);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												GetRandom()-GetRandom())+velBias*.5f) * 4.f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(1.f + GetRandom()*GetRandom()*0.4f,
							   10.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(.1f + GetRandom()*0.02f, 0.f, .10f);
				localEntities.emplace_back(ent);
			}
			
			// slow smoke
			color.w = .15f;
			for(int i = 0; i < 8; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 20.f);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												(GetRandom()-GetRandom()) * .2f)) * 2.f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(1.4f + GetRandom()*GetRandom()*0.8f,
							   0.2f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				if(cg_reduceSmoke)
					ent->SetLifeTime(1.f + GetRandom() * 2.f, 0.1f, 8.f);
				else
					ent->SetLifeTime(4.f + GetRandom() * 5.f, 0.1f, 8.f);
				localEntities.emplace_back(ent);
			}
			
			// fragments
			Handle<IImage> img = renderer->RegisterImage("Gfx/White.tga");
			color = MakeVector4(0.01, 0.03, 0, 1.f);
			for(int i = 0; i < 42; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				Vector3 dir = MakeVector3(GetRandom()-GetRandom(),
										  GetRandom()-GetRandom(),
										  GetRandom()-GetRandom());
				dir += velBias * .5f;
				float radius = 0.1f + GetRandom()*GetRandom()*0.2f;
				ent->SetTrajectory(origin + dir * .2f,
								   dir * 20.f,
								   .1f + radius * 3.f, 1.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(radius);
				ent->SetLifeTime(3.5f + GetRandom() * 2.f, 0.f, 1.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::BounceWeak);
				localEntities.emplace_back(ent);
			}
			
			// fire smoke
			color= MakeVector4(1.f, .6f, .2f, 1.f);
			for(int i = 0; i < 4; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 60.f);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												GetRandom()-GetRandom())+velBias) * 12.f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(1.f + GetRandom()*GetRandom()*0.4f,
							   6.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(.08f + GetRandom()*0.03f, 0.f, .10f);
				ent->SetAdditive(true);
				localEntities.emplace_back(ent);
			}
		}
		
		void Client::GrenadeExplosionUnderwater(spades::Vector3 origin){
			float dist = (origin - lastSceneDef.viewOrigin).GetLength();
			if(dist > 170.f)
				return;
			grenadeVibration += 1.5f / (dist + 5.f);
			if(grenadeVibration > 1.f)
				grenadeVibration = 1.f;
			
			Vector3 velBias = {0,0,0};
			
			Vector4 color;
			color = MakeVector4( .95f, .95f, .95f, .6f);
			// water1
			Handle<IImage> img = renderer->RegisterImage("Textures/WaterExpl.png");
			if(cg_reduceSmoke) color.w = .3f;
			for(int i = 0; i < 7; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												-GetRandom()*7.f)) * 2.5f,
								   .3f, .6f);
				ent->SetRotation(0.f);
				ent->SetRadius(1.5f + GetRandom()*GetRandom()*0.4f,
							   1.3f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(3.f + GetRandom()*0.3f, 0.f, .60f);
				localEntities.emplace_back(ent);
			}
			
			// water2
			img = renderer->RegisterImage("Textures/Fluid.png");
			color.w = .9f;
			if(cg_reduceSmoke) color.w = .4f;
			for(int i = 0; i < 16; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												-GetRandom()*10.f)) * 3.5f,
								   1.f, 1.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(0.9f + GetRandom()*GetRandom()*0.4f,
							   0.7f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime(3.f + GetRandom()*0.3f, .7f, .60f);
				localEntities.emplace_back(ent);
			}
			
			// slow smoke
			color.w = .4f;
			if(cg_reduceSmoke) color.w = .2f;
			for(int i = 0; i < 8; i++){
				ParticleSpriteEntity *ent =
				new SmokeSpriteEntity(this, color, 20.f);
				ent->SetTrajectory(origin,
								   (MakeVector3(GetRandom()-GetRandom(),
												GetRandom()-GetRandom(),
												(GetRandom()-GetRandom()) * .2f)) * 2.f,
								   1.f, 0.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(1.4f + GetRandom()*GetRandom()*0.8f,
							   0.2f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Ignore);
				ent->SetLifeTime((cg_reduceSmoke ? 3.f : 6.f) + GetRandom() * 5.f, 0.1f, 8.f);
				localEntities.emplace_back(ent);
			}
			
			// fragments
			img = renderer->RegisterImage("Gfx/White.tga");
			color = MakeVector4(1,1,1, 0.7f);
			for(int i = 0; i < 42; i++){
				ParticleSpriteEntity *ent =
				new ParticleSpriteEntity(this, img, color);
				Vector3 dir = MakeVector3(GetRandom()-GetRandom(),
										  GetRandom()-GetRandom(),
										  -GetRandom() * 3.f);
				dir += velBias * .5f;
				float radius = 0.1f + GetRandom()*GetRandom()*0.2f;
				ent->SetTrajectory(origin + dir * .2f +
								   MakeVector3(0, 0, -1.2f),
								   dir * 13.f,
								   .1f + radius * 3.f, 1.f);
				ent->SetRotation(GetRandom() * (float)M_PI * 2.f);
				ent->SetRadius(radius);
				ent->SetLifeTime(3.5f + GetRandom() * 2.f, 0.f, 1.f);
				ent->SetBlockHitAction(ParticleSpriteEntity::Delete);
				localEntities.emplace_back(ent);
			}
			
			
			// TODO: wave?
		}
		
		

	}
}
