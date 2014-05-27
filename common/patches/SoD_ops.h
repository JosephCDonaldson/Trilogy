
//list of packets we need to encode on the way out:

E(OP_SendCharInfo)
E(OP_ZoneServerInfo)
E(OP_SendAATable)
E(OP_PlayerProfile)
E(OP_ZoneEntry)
E(OP_CharInventory)
E(OP_NewZone)
E(OP_SpawnDoor)
E(OP_GroundSpawn)
E(OP_SendZonepoints)
E(OP_NewSpawn)
E(OP_ZoneSpawns)
E(OP_ItemLinkResponse)
E(OP_ItemPacket)
E(OP_GuildMemberList)
E(OP_Illusion)
E(OP_ManaChange)
E(OP_ClientUpdate)
E(OP_LeadershipExpUpdate)
E(OP_ExpansionInfo)
E(OP_LogServer)
E(OP_Damage)
E(OP_Buff)
E(OP_Action)
E(OP_Consider)
E(OP_CancelTrade)
E(OP_ShopPlayerSell)
E(OP_DeleteItem)
E(OP_ItemVerifyReply)
E(OP_DeleteCharge)
E(OP_MoveItem)
E(OP_OpenNewTasksWindow)
E(OP_BazaarSearch)
E(OP_Trader)
E(OP_TraderBuy)
E(OP_LootItem)
E(OP_Projectile)
E(OP_ReadBook)
E(OP_Stun)
E(OP_ZonePlayerToBind)
E(OP_RaidUpdate)
E(OP_RaidJoin)
E(OP_VetRewardsAvaliable)
E(OP_InspectRequest)
E(OP_GroupInvite)
E(OP_GroupFollow)
E(OP_GroupFollow2)
E(OP_GroupUpdate)
E(OP_GroupCancelInvite)
E(OP_WhoAllResponse)
E(OP_Track)
E(OP_ShopPlayerBuy)
E(OP_PetBuffWindow)
E(OP_OnLevelMessage)
E(OP_Barter)
E(OP_ApplyPoison)
E(OP_TargetBuffs)
E(OP_WearChange)
//list of packets we need to decode on the way in:
D(OP_SetServerFilter)
D(OP_CharacterCreate)
D(OP_ItemLinkClick)
D(OP_ConsiderCorpse)
D(OP_Consider)
D(OP_ClientUpdate)
D(OP_MoveItem)
D(OP_WhoAllRequest)
D(OP_Buff)
D(OP_ShopPlayerSell)
D(OP_Consume)
D(OP_CastSpell)
D(OP_Save)
D(OP_ItemVerifyRequest)
D(OP_GroupInvite)
D(OP_GroupInvite2)
D(OP_GroupFollow)
D(OP_GroupFollow2)
D(OP_GroupDisband)
D(OP_GroupCancelInvite)
D(OP_FindPersonRequest)
D(OP_TraderBuy)
D(OP_LootItem)
D(OP_ReadBook)
D(OP_AugmentInfo)
D(OP_FaceChange)
D(OP_TradeSkillCombine)
D(OP_RaidInvite)
D(OP_InspectRequest)
D(OP_WearChange)
D(OP_ShopPlayerBuy)
D(OP_BazaarSearch)
D(OP_LoadSpellSet)
D(OP_ApplyPoison)
D(OP_DeleteItem)
D(OP_AugmentItem)
D(OP_Bug)
E(OP_MobUpdate)
#undef E
#undef D
