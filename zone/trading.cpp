/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2002 EQEMu Development Team (http://eqemu.org)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/
#include "../common/debug.h"
#include "masterentity.h"
#include "string_ids.h"
#include "../common/string_util.h"
#include "../common/rulesys.h"
#include "quest_parser_collection.h"
#include "worldserver.h"
#include "queryserv.h"

extern WorldServer worldserver;
extern QueryServ* QServ;

// The maximum amount of a single bazaar/barter transaction expressed in copper.
// Equivalent to 2 Million plat
#define MAX_TRANSACTION_VALUE 2000000000
// ##########################################
// Trade implementation
// ##########################################

Trade::Trade(Mob* in_owner)
{
	owner = in_owner;
	Reset();
}

Trade::~Trade()
{
	Reset();
}

void Trade::Reset()
{
	state = TradeNone;
	with_id = 0;
	pp=0; gp=0; sp=0; cp=0;
}

void Trade::SetTradeCash(uint32 in_pp, uint32 in_gp, uint32 in_sp, uint32 in_cp)
{
	pp=in_pp; gp=in_gp; sp=in_sp; cp=in_cp;
}

// Initiate a trade with another mob
// initiate_with specifies whether to start trade with other mob as well
void Trade::Start(uint32 mob_id, bool initiate_with)
{
	Reset();
	state = Trading;
	with_id = mob_id;

	// Autostart on other mob?
	if (initiate_with) {
		Mob* with = With();
		if (with)
			with->trade->Start(owner->GetID(), false);
	}
}

// Add item from a given slot to trade bucket (automatically does bag data too)
void Trade::AddEntity(uint16 trade_slot_id, uint32 stack_size) {
	// TODO: review for inventory saves / consider changing return type to bool so failure can be passed to desync handler

	if (!owner || !owner->IsClient()) {
		// This should never happen
		LogFile->write(EQEMuLog::Debug, "Programming error: NPC's should not call Trade::AddEntity()");
		return;
	}

	// If one party accepted the trade then an item was added, their state needs to be reset
	owner->trade->state = Trading;
	Mob* with = With();
	if (with)
		with->trade->state = Trading;

	// Item always goes into trade bucket from cursor
	Client* client = owner->CastToClient();
	ItemInst* inst = client->GetInv().GetItem(SLOT_CURSOR);

	if (!inst) {
		client->Message(13, "Error: Could not find item on your cursor!");
		return;
	}

	ItemInst* inst2 = client->GetInv().GetItem(trade_slot_id);

	// it looks like the original code attempted to allow stacking...
	// (it just didn't handle partial stack move actions -U)
	if (stack_size > 0) {
		if (!inst->IsStackable() || !inst2 || !inst2->GetItem() || (inst->GetID() != inst2->GetID()) || (stack_size > inst->GetCharges())) {
			client->Kick();
			return;
		}

		uint32 _stack_size = 0;

		if ((stack_size + inst2->GetCharges()) > inst2->GetItem()->StackSize) {
			_stack_size = (stack_size + inst2->GetCharges()) - inst->GetItem()->StackSize;
			inst2->SetCharges(inst2->GetItem()->StackSize);
		}
		else {
			_stack_size = inst->GetCharges() - stack_size;
			inst2->SetCharges(stack_size + inst2->GetCharges());
		}

		_log(TRADING__HOLDER, "%s added partial item '%s' stack (qty: %i) to trade slot %i", owner->GetName(), inst->GetItem()->Name, stack_size, trade_slot_id);

		if (_stack_size > 0)
			inst->SetCharges(_stack_size);
		else
			client->DeleteItemInInventory(SLOT_CURSOR);

		SendItemData(inst2, trade_slot_id);
	}
	else {
		if (inst2 && inst2->GetID()) {
			client->Kick();
			return;
		}
		
		SendItemData(inst, trade_slot_id);

		_log(TRADING__HOLDER, "%s added item '%s' to trade slot %i", owner->GetName(), inst->GetItem()->Name, trade_slot_id);

		client->PutItemInInventory(trade_slot_id, *inst);
		client->DeleteItemInInventory(SLOT_CURSOR);
	}
}

// Retrieve mob the owner is trading with
// Done like this in case 'with' mob goes LD and Mob* becomes invalid
Mob* Trade::With()
{
	return entity_list.GetMob(with_id);
}

// Private Method: Send item data for trade item to other person involved in trade
void Trade::SendItemData(const ItemInst* inst, int16 dest_slot_id)
{
	// @merth: This needs to be redone with new item classes
	Mob* mob = With();
	if (!mob->IsClient())
		return; // Not sending packets to NPCs!

	Client* with = mob->CastToClient();
	Client* trader = owner->CastToClient();
	if (with && with->IsClient()) {
		int16 fromid = 0;
		fromid = trader->GetID();
		with->SendItemPacket(dest_slot_id -IDX_TRADE,inst,ItemPacketTradeView,fromid);
		if (inst->GetItem()->ItemClass == 1) {
			for (uint16 i=0; i<10; i++) {
				uint16 bagslot_id = Inventory::CalcSlotId(dest_slot_id, i);
				const ItemInst* bagitem = trader->GetInv().GetItem(bagslot_id);
				if (bagitem) {
					with->SendItemPacket(bagslot_id-IDX_TRADE,bagitem,ItemPacketTradeView,fromid);
				}
			}
		}

		//safe_delete(outapp);
	}
}

// Audit trade: The part logged is what travels owner -> with
void Trade::LogTrade()
{
	Mob* with = With();
	if (!owner->IsClient() || !with)
		return; // Should never happen

	Client* trader = owner->CastToClient();
	bool logtrade = false;
	int admin_level = 0;
	uint8 item_count = 0;

	if (zone->tradevar != 0) {
		for (uint16 i=3000; i<=3007; i++) {
			if (trader->GetInv().GetItem(i))
				item_count++;
		}

		if (((this->cp + this->sp + this->gp + this->pp)>0) || (item_count>0))
			admin_level = trader->Admin();
		else
			admin_level = 999;

		if (zone->tradevar == 7) {
			logtrade = true;
		}
		else if ((admin_level>=10) && (admin_level<20)) {
			if ((zone->tradevar<8) && (zone->tradevar>5))
				logtrade = true;
		}
		else if (admin_level<=20) {
			if ((zone->tradevar<8) && (zone->tradevar>4))
				logtrade = true;
		}
		else if (admin_level<=80) {
			if ((zone->tradevar<8) && (zone->tradevar>3))
				logtrade = true;
		}
		else if (admin_level<=100){
			if ((zone->tradevar<9) && (zone->tradevar>2))
				logtrade = true;
		}
		else if (admin_level<=150){
			if (((zone->tradevar<8) && (zone->tradevar>1)) || (zone->tradevar==9))
				logtrade = true;
		}
		else if (admin_level<=255){
			if ((zone->tradevar<8) && (zone->tradevar>0))
				logtrade = true;
		}
	}

	if (logtrade == true) {
		char logtext[1000] = {0};
		uint32 cash = 0;
		bool comma = false;

		// Log items offered by owner
		cash = this->cp + this->sp + this->gp + this->pp;
		if ((cash>0) || (item_count>0)) {
			sprintf(logtext, "%s gave %s ", trader->GetName(), with->GetName());

			if (item_count > 0) {
				strcat(logtext, "items {");

				for (uint16 i=3000; i<=3007; i++) {
					const ItemInst* inst = trader->GetInv().GetItem(i);

					if (!comma)
						comma = true;
					else {
						if (inst)
							strcat(logtext, ",");
					}

					if (inst) {
						char item_num[15] = {0};
						sprintf(item_num, "%i", inst->GetItem()->ID);
						strcat(logtext, item_num);

						if (inst->IsType(ItemClassContainer)) {
							for (uint8 j=0; j<10; j++) {
								inst = trader->GetInv().GetItem(i, j);
								if (inst) {
									strcat(logtext, ",");
									sprintf(item_num, "%i", inst->GetItem()->ID);
									strcat(logtext, item_num);
								}
							}
						}
					}
				}
			}

			if (cash > 0) {
				char money[100] = {0};
				sprintf(money, " %ipp, %igp, %isp, %icp", trader->trade->pp, trader->trade->gp, trader->trade->sp, trader->trade->cp);
				strcat(logtext, money);
			}

			database.logevents(trader->AccountName(), trader->AccountID(),
				trader->Admin(), trader->GetName(), with->GetName(), "Trade", logtext, 6);
		}
	}
}

#if (EQDEBUG >= 9)
void Trade::DumpTrade()
{
	Mob* with = With();
	LogFile->write(EQEMuLog::Debug, "Dumping trade data: '%s' in TradeState %i with '%s'",
		this->owner->GetName(), state, ((with==nullptr)?"(null)":with->GetName()));

	if (!owner->IsClient())
		return;

	Client* trader = owner->CastToClient();
	for (uint16 i=3000; i<=3007; i++) {
		const ItemInst* inst = trader->GetInv().GetItem(i);

		if (inst) {
			LogFile->write(EQEMuLog::Debug, "Item %i (Charges=%i, Slot=%i, IsBag=%s)",
				inst->GetItem()->ID, inst->GetCharges(),
				i, ((inst->IsType(ItemClassContainer)) ? "True" : "False"));

			if (inst->IsType(ItemClassContainer)) {
				for (uint8 j=0; j<10; j++) {
					inst = trader->GetInv().GetItem(i, j);
					if (inst) {
						LogFile->write(EQEMuLog::Debug, "\tBagItem %i (Charges=%i, Slot=%i)",
							inst->GetItem()->ID, inst->GetCharges(),
							Inventory::CalcSlotId(i, j));
					}
				}
			}
		}
	}

	LogFile->write(EQEMuLog::Debug, "\tpp:%i, gp:%i, sp:%i, cp:%i", pp, gp, sp, cp);
}
#endif

void Client::ResetTrade() {
	const Item_Struct* TempItem = 0;
	ItemInst* ins;
	int x;
	AddMoneyToPP(trade->cp, trade->sp, trade->gp, trade->pp, true);
	for (x = 3000; x <= 3007; x++)
	{
		TempItem = 0;
		ins = GetInv().GetItem(x);
		if (ins)
			TempItem = ins->GetItem();
		if (TempItem)
		{
			bool is_arrow = (TempItem->ItemType == ItemTypeArrow) ? true : false;
			int freeslotid = GetInv().FindFreeSlot(ins->IsType(ItemClassContainer), true, TempItem->Size, is_arrow);
			if (freeslotid == SLOT_INVALID)
			{
				DropInst(ins);
			}
			else
			{
				PutItemInInventory(freeslotid, *ins);
				SendItemPacket(freeslotid, ins, ItemPacketTrade);
			}
			DeleteItemInInventory(x);
		}
	}
}

void Client::FinishTrade(Mob* tradingWith, bool finalizer, void* event_entry, std::list<void*>* event_details) {

	if (tradingWith && tradingWith->IsClient()) {
		Client* other = tradingWith->CastToClient();
		QSPlayerLogTrade_Struct* qsaudit = nullptr;
		bool QSPLT = false;

		if (other) {
			mlog(TRADING__CLIENT, "Finishing trade with client %s", other->GetName());

			int16 slot_id;
			const Item_Struct* item = nullptr;

			// QS code
			if (RuleB(QueryServ, PlayerLogTrades) && event_entry && event_details) {
				qsaudit = (QSPlayerLogTrade_Struct*)event_entry;
				QSPLT = true;

				if (finalizer) { qsaudit->char2_id = this->character_id; }
				else { qsaudit->char1_id = this->character_id; }
			}

			// Move each trade slot into free inventory slot
			for (int16 i = 3000; i <= 3007; i++){
				const ItemInst* inst = m_inv[i];
				uint16 parent_offset = 0;

				if (inst == nullptr) { continue; }

				mlog(TRADING__CLIENT, "Giving %s (%d) in slot %d to %s", inst->GetItem()->Name, inst->GetItem()->ID, i, other->GetName());

				/// Log Player Trades through QueryServ if Rule Enabled
				if (QSPLT) {
					uint16 item_count = qsaudit->char1_count + qsaudit->char2_count;
					parent_offset = item_count;

					qsaudit->items[item_count].from_id = this->character_id;
					qsaudit->items[item_count].from_slot = i;
					qsaudit->items[item_count].to_id = other->CharacterID();
					qsaudit->items[item_count].to_slot = 0;
					qsaudit->items[item_count].item_id = inst->GetID();
					qsaudit->items[item_count].charges = inst->GetCharges();

					if (finalizer) { qsaudit->char2_count++; }
					else { qsaudit->char1_count++; }

					if (inst->IsType(ItemClassContainer)) {
						// Pseudo-Slot ID's are generated based on how the db saves bag items...
						for (uint8 j = 0; j < inst->GetItem()->BagSlots; j++) {
							const ItemInst* baginst = inst->GetItem(j);

							if (baginst == nullptr) { continue; }

							int16 k = Inventory::CalcSlotId(i, j);
							item_count = qsaudit->char1_count + qsaudit->char2_count;

							qsaudit->items[item_count].from_id = this->character_id;
							qsaudit->items[item_count].from_slot = k;
							qsaudit->items[item_count].to_id = other->CharacterID();
							qsaudit->items[item_count].to_slot = 0;
							qsaudit->items[item_count].item_id = baginst->GetID();
							qsaudit->items[item_count].charges = baginst->GetCharges();

							if (finalizer) { qsaudit->char2_count++; }
							else { qsaudit->char1_count++; }
						}
					}
				}

				if (inst->GetItem()->NoDrop != 0 || Admin() >= RuleI(Character, MinStatusForNoDropExemptions) || RuleI(World, FVNoDropFlag) == 1 || other == this) {
					bool is_arrow = (inst->GetItem()->ItemType == ItemTypeArrow) ? true : false;
					slot_id = other->GetInv().FindFreeSlot(inst->IsType(ItemClassContainer), true, inst->GetItem()->Size, is_arrow);

					mlog(TRADING__CLIENT, "Trying to put %s (%d) into slot %d", inst->GetItem()->Name, inst->GetItem()->ID, slot_id);

					if (other->PutItemInInventory(slot_id, *inst, true)) {
						mlog(TRADING__CLIENT, "Item %s (%d) successfully transfered, deleting from trade slot.", inst->GetItem()->Name, inst->GetItem()->ID);

						if (QSPLT) {
							qsaudit->items[parent_offset].to_slot = slot_id;

							if (inst->IsType(ItemClassContainer)) {
								for (uint8 bagslot_idx = 0; bagslot_idx < inst->GetItem()->BagSlots; bagslot_idx++) {
									const ItemInst* bag_inst = inst->GetItem(bagslot_idx);

									if (bag_inst == nullptr) { continue; }
									int16 to_bagslot_id = Inventory::CalcSlotId(slot_id, bagslot_idx);

									qsaudit->items[++parent_offset].to_slot = to_bagslot_id;
								}
							}
						}
					}
					else {
						PushItemOnCursor(*inst, true);
						mlog(TRADING__ERROR, "Unable to give item %d (%d) to %s, returning to giver.", inst->GetItem()->Name, inst->GetItem()->ID, other->GetName());

						if (QSPLT) {
							qsaudit->items[parent_offset].to_id = this->character_id;
							qsaudit->items[parent_offset].to_slot = SLOT_CURSOR;

							if (inst->IsType(ItemClassContainer)) {
								for (uint8 bagslot_idx = 0; bagslot_idx < inst->GetItem()->BagSlots; bagslot_idx++) {
									const ItemInst* bag_inst = inst->GetItem(bagslot_idx);

									if (bag_inst == nullptr) { continue; }
									int16 to_bagslot_id = Inventory::CalcSlotId(SLOT_CURSOR, bagslot_idx);

									qsaudit->items[++parent_offset].to_id = this->character_id;
									qsaudit->items[parent_offset].to_slot = to_bagslot_id;
								}
							}
						}
					}

					DeleteItemInInventory(i);
				}
				else {
					PushItemOnCursor(*inst, true);
					DeleteItemInInventory(i);

					if (QSPLT) {
						qsaudit->items[parent_offset].to_id = this->character_id;
						qsaudit->items[parent_offset].to_slot = SLOT_CURSOR;

						if (inst->IsType(ItemClassContainer)) {
							for (uint8 bagslot_idx = 0; bagslot_idx < inst->GetItem()->BagSlots; bagslot_idx++) {
								const ItemInst* bag_inst = inst->GetItem(bagslot_idx);

								if (bag_inst == nullptr) { continue; }
								int16 to_bagslot_id = Inventory::CalcSlotId(SLOT_CURSOR, bagslot_idx);

								qsaudit->items[++parent_offset].to_id = this->character_id;
								qsaudit->items[parent_offset].to_slot = to_bagslot_id;
							}
						}
					}
				}
			}

			// Money - look into how NPC's receive cash
			this->AddMoneyToPP(other->trade->cp, other->trade->sp, other->trade->gp, other->trade->pp, true);

			// This is currently setup to show character offers, not receipts
			if (QSPLT) {
				if (finalizer) {
					qsaudit->char2_money.platinum = this->trade->pp;
					qsaudit->char2_money.gold = this->trade->gp;
					qsaudit->char2_money.silver = this->trade->sp;
					qsaudit->char2_money.copper = this->trade->cp;
				}
				else {
					qsaudit->char1_money.platinum = this->trade->pp;
					qsaudit->char1_money.gold = this->trade->gp;
					qsaudit->char1_money.silver = this->trade->sp;
					qsaudit->char1_money.copper = this->trade->cp;
				}
			}

			//Do not reset the trade here, done by the caller.
		}
	}
	else if (tradingWith && tradingWith->IsNPC()) {
		QSPlayerLogHandin_Struct* qsaudit = nullptr;
		bool QSPLH = false;

		// QS code
		if (RuleB(QueryServ, PlayerLogTrades) && event_entry && event_details) {
			// Currently provides only basic functionality. Calling method will also
			// need to be modified before item returns and rewards can be logged. -U
			qsaudit = (QSPlayerLogHandin_Struct*)event_entry;
			QSPLH = true;

			qsaudit->quest_id = 0;
			qsaudit->char_id = character_id;
			qsaudit->char_money.platinum = trade->pp;
			qsaudit->char_money.gold = trade->gp;
			qsaudit->char_money.silver = trade->sp;
			qsaudit->char_money.copper = trade->cp;
			qsaudit->char_count = 0;
			qsaudit->npc_id = tradingWith->GetNPCTypeID();
			qsaudit->npc_money.platinum = 0;
			qsaudit->npc_money.gold = 0;
			qsaudit->npc_money.silver = 0;
			qsaudit->npc_money.copper = 0;
			qsaudit->npc_count = 0;
		}

		if (QSPLH) { // This can be incoporated below when revisions are made -U
			for (int16 slot_id = 3000; slot_id <= 3003; slot_id++) {
				const ItemInst* trade_inst = m_inv[slot_id];

				if (trade_inst) {
					strcpy(qsaudit->items[qsaudit->char_count].action_type, "HANDIN");

					qsaudit->items[qsaudit->char_count].char_slot = slot_id;
					qsaudit->items[qsaudit->char_count].item_id = trade_inst->GetID();
					qsaudit->items[qsaudit->char_count++].charges = trade_inst->GetCharges();

					if (trade_inst->IsType(ItemClassContainer)) {
						for (uint8 bag_idx = 0; bag_idx < trade_inst->GetItem()->BagSlots; bag_idx++) {
							const ItemInst* trade_baginst = trade_inst->GetItem(bag_idx);

							if (trade_baginst) {
								strcpy(qsaudit->items[qsaudit->char_count].action_type, "HANDIN");

								qsaudit->items[qsaudit->char_count].char_slot = Inventory::CalcSlotId(slot_id, bag_idx);
								qsaudit->items[qsaudit->char_count].item_id = trade_baginst->GetID();
								qsaudit->items[qsaudit->char_count++].charges = trade_baginst->GetCharges();
							}
						}
					}
				}
			}
		}

		bool quest_npc = false;
		if (parse->HasQuestSub(tradingWith->GetNPCTypeID(), EVENT_TRADE)) {
			// This is a quest NPC
			quest_npc = true;
		}

		std::vector<EQEmu::Any> item_list;
		uint32 items[4] = { 0 };
		for (int i = 3000; i < 3004; ++i) {
			ItemInst *inst = m_inv.GetItem(i);
			if (inst) {
				items[i - 3000] = inst->GetItem()->ID;
				item_list.push_back(inst);
			}
			else {
				item_list.push_back(nullptr);
				continue;
			}

			const Item_Struct* item = inst->GetItem();
			if (item && quest_npc == false) {
				// if it was not a NO DROP or Attuned item (or if a GM is trading), let the NPC have it
				if (GetGM() || (item->NoDrop != 0 && inst->IsInstNoDrop() == false)) {
					// pets need to look inside bags and try to equip items found there
					if (item->ItemClass == ItemClassContainer && item->BagSlots > 0) {
						for (int16 bslot = 0; bslot < item->BagSlots; bslot++) {
							const ItemInst* baginst = inst->GetItem(bslot);
							if (baginst) {
								const Item_Struct* bagitem = baginst->GetItem();
								if (bagitem && (GetGM() || (bagitem->NoDrop != 0 && baginst->IsInstNoDrop() == false))) {
									tradingWith->CastToNPC()->AddLootDrop(bagitem, &tradingWith->CastToNPC()->itemlist,
										baginst->GetCharges(), 1, 127, true, true);
								}
								else if (RuleB(NPC, ReturnNonQuestNoDropItems)) {
									PushItemOnCursor(*baginst, true);
								}
							}
						}
					}

					tradingWith->CastToNPC()->AddLootDrop(item, &tradingWith->CastToNPC()->itemlist,
						inst->GetCharges(), 1, 127, true, true);
				}
				// Return NO DROP and Attuned items being handed into a non-quest NPC if the rule is true
				else if (RuleB(NPC, ReturnNonQuestNoDropItems)) {
					PushItemOnCursor(*inst, true);
					DeleteItemInInventory(i);
				}
			}
		}

		char temp1[100] = { 0 };
		char temp2[100] = { 0 };
		snprintf(temp1, 100, "copper.%d", tradingWith->GetNPCTypeID());
		snprintf(temp2, 100, "%u", trade->cp);
		parse->AddVar(temp1, temp2);
		snprintf(temp1, 100, "silver.%d", tradingWith->GetNPCTypeID());
		snprintf(temp2, 100, "%u", trade->sp);
		parse->AddVar(temp1, temp2);
		snprintf(temp1, 100, "gold.%d", tradingWith->GetNPCTypeID());
		snprintf(temp2, 100, "%u", trade->gp);
		parse->AddVar(temp1, temp2);
		snprintf(temp1, 100, "platinum.%d", tradingWith->GetNPCTypeID());
		snprintf(temp2, 100, "%u", trade->pp);
		parse->AddVar(temp1, temp2);

		if (tradingWith->GetAppearance() != eaDead) {
			tradingWith->FaceTarget(this);
		}

		ItemInst *insts[4] = { 0 };
		for (int i = 3000; i < 3004; ++i) {
			insts[i - 3000] = m_inv.PopItem(i);
			database.SaveInventory(CharacterID(), nullptr, i);
		}

		parse->EventNPC(EVENT_TRADE, tradingWith->CastToNPC(), this, "", 0, &item_list);

		for (int i = 0; i < 4; ++i) {
			if (insts[i]) {
				safe_delete(insts[i]);
			}
		}
	}
}

bool Client::CheckTradeLoreConflict(Client* other)
{
	if (!other)
		return true;
	// Move each trade slot into free inventory slot
	for (int16 i = 3000; i <= 3007; i++){
		const ItemInst* inst = m_inv[i];

		if (inst && inst->GetItem()) {
			if (other->CheckLoreConflict(inst->GetItem()))
				return true;
		}
	}

	for (int16 i = 3000; i <= 3007; i++){
		const ItemInst* inst = m_inv[i];

		if (inst && inst->GetItem()) {
			if (other->CheckLoreConflict(inst->GetItem()))
				return true;
		}
	}

	return false;
}

void Client::Trader_ShowItems(){
	EQApplicationPacket* outapp= new EQApplicationPacket(OP_Trader, sizeof(Trader_Struct));

	Trader_Struct* outints = (Trader_Struct*)outapp->pBuffer;
	Trader_Struct* TraderItems = database.LoadTraderItem(this->CharacterID());

	for(int i = 0; i < 80; i++){
		outints->ItemCost[i] = TraderItems->ItemCost[i];
		outints->Items[i] = TraderItems->Items[i];
	}
	outints->Code = BazaarTrader_ShowItems;

	QueuePacket(outapp);
	_pkt(TRADING__PACKETS, outapp);
	safe_delete(outapp);
	safe_delete(TraderItems);
}

void Client::SendTraderPacket(Client* Trader, uint32 Unknown72)
{
	if(!Trader)
		return;

	EQApplicationPacket* outapp= new EQApplicationPacket(OP_BecomeTrader, sizeof(BecomeTrader_Struct));

	BecomeTrader_Struct* bts = (BecomeTrader_Struct*)outapp->pBuffer;

	bts->Code = BazaarTrader_StartTraderMode;

	bts->ID = Trader->GetID();

	strn0cpy(bts->Name, Trader->GetName(), sizeof(bts->Name));

	bts->Unknown072 = Unknown72;

	QueuePacket(outapp);

	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);
}

void Client::Trader_CustomerBrowsing(Client *Customer) {

	//Not working atm, need to figure out packet.
	EQApplicationPacket* outapp= new EQApplicationPacket(OP_Trader, sizeof(Trader_ShowItems_Struct));
	Trader_ShowItems_Struct* sis = (Trader_ShowItems_Struct*)outapp->pBuffer;
	sis->Code = BazaarTrader_CustomerBrowsing;
	sis->TraderID = Customer->GetID();
	QueuePacket(outapp);
}

void Client::Trader_CustomerBought(Client *Customer,uint32 Price, uint32 ItemID, uint32 Quantity, const char* ItemName) {

	EQApplicationPacket* outapp= new EQApplicationPacket(OP_Trader, sizeof(TraderBuy_Struct));
	TraderBuy_Struct* sis = (TraderBuy_Struct*)outapp->pBuffer;

	sis->Action = BazaarBuyItem;
	sis->ItemID = ItemID;
	sis->Price = Price;
	sis->Quantity = Quantity;
	strcpy(sis->ItemName,ItemName);
	QueuePacket(outapp);
}

void Client::Trader_StartTrader() {

	Trader=true;

	EQApplicationPacket* outapp= new EQApplicationPacket(OP_Trader, sizeof(Trader_ShowItems_Struct));

	Trader_ShowItems_Struct* sis = (Trader_ShowItems_Struct*)outapp->pBuffer;

	sis->Code = BazaarTrader_StartTraderMode;

	sis->TraderID = this->GetID();

	QueuePacket(outapp);

	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);

	// Notify other clients we are now in trader mode

	outapp= new EQApplicationPacket(OP_BecomeTrader, sizeof(BecomeTrader_Struct));

	BecomeTrader_Struct* bts = (BecomeTrader_Struct*)outapp->pBuffer;

	bts->Code = BazaarTrader_StartTraderMode;

	bts->ID = this->GetID();

	strn0cpy(bts->Name, GetName(), sizeof(bts->Name));

	entity_list.QueueClients(this, outapp, false);

	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);
}

void Client::Trader_EndTrader() {

	// If someone is looking at our wares, remove all the items from the window.
	//
	if(CustomerID) {
		Client* Customer = entity_list.GetClientByID(CustomerID);
		GetItems_Struct* gis=GetTraderItems();

		if(Customer && gis) {
			EQApplicationPacket* outapp = new EQApplicationPacket(OP_TraderDelItem,sizeof(TraderDelItem_Struct));
			TraderDelItem_Struct* tdis = (TraderDelItem_Struct*)outapp->pBuffer;

			tdis->Unknown000 = 0;
			tdis->TraderID = Customer->GetID();
			tdis->Unknown012 = 0;
			Customer->Message(13, "The Trader is no longer open for business");

			for(int i = 0; i < 80; i++) {
				if(gis->Items[i] != 0) {

					tdis->ItemID = gis->SerialNumber[i];

					Customer->QueuePacket(outapp);
				}
			}

			safe_delete(outapp);
			safe_delete(gis);

			EQApplicationPacket empty(OP_ShopEndConfirm);
			Customer->QueuePacket(&empty);
			Customer->Save();
		}
	}

	database.DeleteTraderItem(this->CharacterID());

	// Notify other clients we are no longer in trader mode.
	//
	EQApplicationPacket* outapp= new EQApplicationPacket(OP_BecomeTrader, sizeof(BecomeTrader_Struct));

	BecomeTrader_Struct* bts = (BecomeTrader_Struct*)outapp->pBuffer;

	bts->Code = 0;

	bts->ID = this->GetID();

	strn0cpy(bts->Name, GetName(), sizeof(bts->Name));

	entity_list.QueueClients(this, outapp, false);

	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);

	outapp= new EQApplicationPacket(OP_Trader, sizeof(Trader_ShowItems_Struct));

	Trader_ShowItems_Struct* sis = (Trader_ShowItems_Struct*)outapp->pBuffer;

	sis->Code = BazaarTrader_EndTraderMode;

	sis->TraderID = BazaarTrader_EndTraderMode;

	QueuePacket(outapp);

	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);

	WithCustomer(0);

	this->Trader = false;
}

void Client::SendTraderItem(uint32 ItemID, uint16 Quantity) {

	std::string Packet;
	int16 FreeSlotID=0;

	const Item_Struct* item = database.GetItem(ItemID);

	if(!item){
		_log(TRADING__CLIENT, "Bogus item deleted in Client::SendTraderItem!\n");
		return;
	}

	ItemInst* inst = database.CreateItem(item, Quantity);

	if (inst)
	{
		bool is_arrow = (inst->GetItem()->ItemType == ItemTypeArrow) ? true : false;
		FreeSlotID = m_inv.FindFreeSlot(false, true, inst->GetItem()->Size, is_arrow);

		PutItemInInventory(FreeSlotID, *inst);
		Save();

		SendItemPacket(FreeSlotID, inst, ItemPacketTrade);

		safe_delete(inst);
	}
}

void Client::SendSingleTraderItem(uint32 CharID, int SerialNumber) {

	ItemInst* inst= database.LoadSingleTraderItem(CharID, SerialNumber);

	BulkSendTraderInventory(CharID);

}

void Client::BulkSendTraderInventory(uint32 char_id) {
	const Item_Struct *item;

	TraderCharges_Struct* TraderItems = database.LoadTraderItemWithCharges(char_id);

	uint32 size = 0;
	std::map<uint16, std::string> ser_items;
	std::map<uint16, std::string>::iterator mer_itr;
	for (uint8 i = 0;i < 80; i++) {
		if((TraderItems->ItemID[i] == 0) || (TraderItems->ItemCost[i] <= 0))
			continue;
		else
			item=database.GetItem(TraderItems->ItemID[i]);
		if (item && (item->NoDrop!=0)) {
			ItemInst* inst = database.CreateItem(item);
			if (inst) {
				inst->SetSerialNumber(TraderItems->SerialNumber[i]);
				if(TraderItems->Charges[i] > 0)
					inst->SetCharges(TraderItems->Charges[i]);

				if(inst->IsStackable()) {
					inst->SetMerchantCount(TraderItems->Charges[i]);
					inst->SetMerchantSlot(TraderItems->SerialNumber[i]);
				}
				inst->SetMerchantSlot(i);
				inst->SetPrice(TraderItems->ItemCost[i]);
					std::string packet = inst->Serialize(30);
					ser_items[i] = packet;
					size += packet.length();
			}
			else
				_log(TRADING__CLIENT, "Client::BulkSendTraderInventory nullptr inst pointer");
		}
		else
			_log(TRADING__CLIENT, "Client::BulkSendTraderInventory nullptr item pointer or item is NODROP %8X",item);
	}
		int8 count = 0;
		EQApplicationPacket* outapp = new EQApplicationPacket(OP_ShopInventoryPacket, size);
		uchar* ptr = outapp->pBuffer;
		for(mer_itr = ser_items.begin(); mer_itr != ser_items.end(); mer_itr++){
			int length = mer_itr->second.length();
			if(length > 5) {
				memcpy(ptr, mer_itr->second.c_str(), length);
				ptr += length;
				count++;
			}
			if(count >= 79)
				break;
		}

		QueuePacket(outapp);
		safe_delete(outapp);
	safe_delete(TraderItems);
}

ItemInst* Client::FindTraderItemBySerialNumber(int32 SerialNumber){

	ItemInst* item = nullptr;
	uint16 SlotID = 0;
	for(int i = 22; i <= 29; i++){
		item = this->GetInv().GetItem(i);
		if(item && item->GetItem()->ID == 17899){ //Traders Satchel
			for(int x = 0; x < 10; x++) {
				// we already have the parent bag and a contents iterator..why not just iterate the bag!??
				SlotID = Inventory::CalcSlotId(i, x);
				item = this->GetInv().GetItem(SlotID);
				if(item) {
					if(item->GetSerialNumber() == SerialNumber)
						return item;
				}
			}
		}
	}
	_log(TRADING__CLIENT, "Client::FindTraderItemBySerialNumber Couldn't find item! Serial No. was %i", SerialNumber);

	return nullptr;
}

ItemInst* Client::FindTraderItemByID(int32 ItemID){

	ItemInst* item = nullptr;
	uint16 SlotID = 0;
	for(int i = 0; i < 8;i++){
		item = this->GetInv().GetItem(22 + i);
		if(item && item->GetItem()->ID == 17899){ //Traders Satchel
			for(int x = 0; x < 10; x++){
				SlotID = (((22 + i + 3) * 10) + x + 1);
				item = this->GetInv().GetItem(SlotID);
				if(item && item->GetItem()->ID == ItemID)
					return item;
			}
		}
	}
	_log(TRADING__CLIENT, "Client::FindTraderItemByID Couldn't find item! Item No. was %i", ItemID);

	return nullptr;
}

GetItems_Struct* Client::GetTraderItems(){

	const ItemInst* item = nullptr;
	uint16 SlotID = 0;

	GetItems_Struct* gis= new GetItems_Struct;

	memset(gis,0,sizeof(GetItems_Struct));

	uint8 ndx = 0;

	for(int i = 22; i <= 29; i++) {
		item = this->GetInv().GetItem(i);
		if(item && item->GetItem()->ID == 17899){ //Traders Satchel
			for(int x = 0; x < 10; x++) {
				SlotID = Inventory::CalcSlotId(i, x);

				item = this->GetInv().GetItem(SlotID);

				if(item){
					gis->Items[ndx] = item->GetItem()->ID;
					gis->SerialNumber[ndx] = item->GetSerialNumber();
					gis->Charges[ndx] = item->GetCharges();
					ndx++;
				}
			}
		}
	}
	return gis;
}

uint16 Client::FindTraderItem(int32 ItemID, uint16 Quantity){

	const ItemInst* item= nullptr;
	uint16 SlotID = 0;
	for(int i = 22; i <= 29; i++) {
		item = this->GetInv().GetItem(i);
		if(item && item->GetItem()->ID == 17899){ //Traders Satchel
			for(int x = 0; x < 10; x++){
				SlotID = Inventory::CalcSlotId(i, x);

				item = this->GetInv().GetItem(SlotID);

				if(item && item->GetID() == ItemID &&
					(item->GetCharges() >= Quantity || (item->GetCharges() <= 0 && Quantity == 1))){

					return SlotID;
				}
			}
		}
	}
	_log(TRADING__CLIENT, "Could NOT find a match for Item: %i with a quantity of: %i on Trader: %s\n",
					ItemID , Quantity, this->GetName());

	return 0;
}

void Client::NukeTraderItem(uint16 Slot,int16 Charges,uint16 Quantity,Client* Customer,uint16 TraderSlot, int SerialNumber, uint32 SellerID) {

	Client* Seller = entity_list.GetClientByID(SellerID);
	TraderCharges_Struct* gis = database.LoadTraderItemWithCharges(Seller->CharacterID());

	if(!Customer) return;
	_log(TRADING__CLIENT, "NukeTraderItem(Slot %i, Charges %i, Quantity %i TraderSlot %i", Slot, Charges, Quantity, TraderSlot);
	if(Quantity < Charges) {
		Customer->SendSingleTraderItem(this->CharacterID(), SerialNumber);
		m_inv.DeleteItem(Slot, Quantity);
	}
	else
	{
		EQApplicationPacket* outapp = new EQApplicationPacket(OP_ShopDelItem, sizeof(Merchant_DelItem_Struct));
		Merchant_DelItem_Struct* tdis = (Merchant_DelItem_Struct*)outapp->pBuffer;
		tdis->playerid = 0;
		tdis->npcid = Customer->GetID();
		tdis->type=65;
		tdis->itemslot = TraderSlot;

		_log(TRADING__CLIENT, "Telling customer to remove item %i with %i charges.",SerialNumber, Charges);
		_pkt(TRADING__PACKETS, outapp);

		Customer->QueuePacket(outapp);
		safe_delete(outapp);

		m_inv.DeleteItem(Slot);
	}
	// This updates the trader. Removes it from his trading bags.
	//
	const ItemInst* Inst = m_inv[Slot];

	database.SaveInventory(CharacterID(), Inst, Slot);

	EQApplicationPacket* outapp2;

	if(Quantity < Charges)
		outapp2 = new EQApplicationPacket(OP_MoveItem,sizeof(MoveItem_Struct));

	MoveItem_Struct* mis = (MoveItem_Struct*)outapp2->pBuffer;
	mis->from_slot = Slot;
	mis->to_slot = 0xFFFFFFFF;
	mis->number_in_stack = 0xFFFFFFFF;

	if(Quantity >= Charges)
		Quantity = 1;

	for(int i = 0; i < Quantity; i++) {
		_pkt(TRADING__PACKETS, outapp2);

		this->QueuePacket(outapp2);
	}
	safe_delete(outapp2);

}
void Client::TraderUpdate(uint16 SlotID,uint32 TraderID){
	// This method is no longer used.

	EQApplicationPacket* outapp = new EQApplicationPacket(OP_TraderItemUpdate,sizeof(TraderItemUpdate_Struct));
	TraderItemUpdate_Struct* tus=(TraderItemUpdate_Struct*)outapp->pBuffer;
	tus->Charges = 0xFFFF;
	tus->FromSlot = SlotID;
	tus->ToSlot = 0xFF;
	tus->TraderID = TraderID;
	tus->Unknown000 = 0;
	QueuePacket(outapp);
	safe_delete(outapp);
}

void Client::FindAndNukeTraderItem(int32 ItemID, uint16 Quantity, Client* Customer, uint16 SlotID){

	uint32 SellerID = this->GetID();
	const ItemInst* item= nullptr;
	bool Stackable = false;
	int16 Charges=0;

	if(SlotID > 0){

		item = this->GetInv().GetItem(SlotID);

		if(item) {
			Charges = this->GetInv().GetItem(SlotID)->GetCharges();

			Stackable = item->IsStackable();

			if(!Stackable)
				Quantity = (Charges > 0) ? Charges : 1;

			_log(TRADING__CLIENT, "FindAndNuke %s, Charges %i, Quantity %i", item->GetItem()->Name, Charges, Quantity);
		}
		if(item && (Charges <= Quantity || (Charges <= 0 && Quantity==1) || !Stackable)){
			this->DeleteItemInInventory(SlotID, Quantity);

			TraderCharges_Struct* GetSlot = database.LoadTraderItemWithCharges(this->CharacterID());

			uint8 Count = 0;

			bool TestSlot = true;

			for(int y = 0;y < 80;y++){

				if(TestSlot && GetSlot->ItemID[y] == ItemID){

					database.DeleteTraderItem(this->CharacterID(),y);
					NukeTraderItem(SlotID, Charges, Quantity, Customer,y,GetSlot->ItemID[y], SellerID);
					TestSlot=false;
				}
				else if(GetSlot->ItemID[y] > 0)
					Count++;
			}
			if(Count == 0)
				Trader_EndTrader();

			return;
		}
		else if(item) {
			TraderCharges_Struct* GetSlot = database.LoadTraderItemWithCharges(this->CharacterID());

			for(int y = 0;y < 80;y++){
				if(GetSlot->ItemID[y] == ItemID){
					database.UpdateTraderItemCharges(this->CharacterID(), item->GetSerialNumber(), Charges-Quantity);
					NukeTraderItem(SlotID, Charges, Quantity, Customer,y,GetSlot->ItemID[y], SellerID);
					return;
				}
			}
		}
	}
	_log(TRADING__CLIENT, "Could NOT find a match for Item: %i with a quantity of: %i on Trader: %s\n",ItemID,
					Quantity,this->GetName());
}

void Client::ReturnTraderReq(const EQApplicationPacket* app, int16 TraderItemCharges, int TraderSlot, uint32 Price){

	TraderBuy_Struct* tbs = (TraderBuy_Struct*)app->pBuffer;

	EQApplicationPacket* outapp = new EQApplicationPacket(OP_TraderBuy, sizeof(TraderBuy_Struct));

	TraderBuy_Struct* outtbs = (TraderBuy_Struct*)outapp->pBuffer;

	memcpy(outtbs, tbs, app->size);

	outtbs->Price = Price;

	outtbs->Quantity = TraderItemCharges;

	outtbs->TraderID = this->GetID();

	outtbs->AlreadySold = TraderSlot;

	QueuePacket(outapp);

	safe_delete(outapp);
}

void Client::TradeRequestFailed(const EQApplicationPacket* app) {

	TraderBuy_Struct* tbs = (TraderBuy_Struct*)app->pBuffer;

	EQApplicationPacket* outapp = new EQApplicationPacket(OP_TraderBuy, sizeof(TraderBuy_Struct));

	TraderBuy_Struct* outtbs = (TraderBuy_Struct*)outapp->pBuffer;

	memcpy(outtbs, tbs, app->size);

	outtbs->AlreadySold = 0xFFFFFFFF;

	outtbs->TraderID = 0xFFFFFFFF;

	QueuePacket(outapp);

	safe_delete(outapp);
}


static void BazaarAuditTrail(const char *Seller, const char *Buyer, const char *ItemName, int Quantity, int TotalCost, int TranType) {

	const char *AuditQuery="INSERT INTO `trader_audit` (`time`, `seller`, `buyer`, `itemname`, `quantity`, `totalcost`, `trantype`) "
				"VALUES (NOW(), '%s', '%s', '%s', %i, %i, %i)";

	char errbuf[MYSQL_ERRMSG_SIZE];
	char* query = 0;

	if(!database.RunQuery(query, MakeAnyLenString(&query, AuditQuery, Seller, Buyer, ItemName, Quantity, TotalCost, TranType), errbuf))
		_log(TRADING__CLIENT, "Audit write error: %s : %s", query, errbuf);

	safe_delete_array(query);
}



void Client::BuyTraderItem(TraderBuy_Struct* tbs,Client* Trader,const EQApplicationPacket* app){

	if(!Trader) return;

	if(!Trader->IsTrader()) {
		TradeRequestFailed(app);
		return;
	}

	EQApplicationPacket* outapp = new EQApplicationPacket(OP_Trader,sizeof(TraderBuy_Struct));

	TraderBuy_Struct* outtbs = (TraderBuy_Struct*)outapp->pBuffer;

	outtbs->ItemID = tbs->ItemID;

	const ItemInst* BuyItem;
	BuyItem = Trader->FindTraderItemByID(tbs->ItemID);

	if(!BuyItem) {
		_log(TRADING__CLIENT, "Unable to find item on trader.");
		TradeRequestFailed(app);
		safe_delete(outapp);
		return;
	}

	uint32 priceper = tbs->Price / tbs->Quantity;
	outtbs->Price = tbs->Price;
	_log(TRADING__CLIENT, "Buyitem: Name: %s, IsStackable: %i, Requested Quantity: %i, Charges on Item %i Price: %i Slot: %i Price per item: %i",
					BuyItem->GetItem()->Name, BuyItem->IsStackable(), tbs->Quantity, BuyItem->GetCharges(), tbs->Price, tbs->AlreadySold, priceper);
	// If the item is not stackable, then we can only be buying one of them.
	if(!BuyItem->IsStackable())
		outtbs->Quantity = tbs->Quantity;
	else {
		// Stackable items, arrows, diamonds, etc
		int ItemCharges = BuyItem->GetCharges();

		// ItemCharges for stackables should not be <= 0
		if(ItemCharges <= 0)
			outtbs->Quantity = 1;
		// If the purchaser requested more than is in the stack, just sell them how many are actually in the stack.
		else if(ItemCharges < (int16)tbs->Quantity)
		{
			outtbs->Price =  tbs->Price - ((tbs->Quantity - ItemCharges) * priceper);
			outtbs->Quantity = ItemCharges;
		}
		else
			outtbs->Quantity = tbs->Quantity;
	}

	_log(TRADING__CLIENT, "Actual quantity that will be traded is %i for cost: %i", outtbs->Quantity, outtbs->Price);

	if(outtbs->Price <= 0) {
		Message(13, "Internal error. Aborting trade. Please report this to the ServerOP. Error code is 1");
		Trader->Message(13, "Internal error. Aborting trade. Please report this to the ServerOP. Error code is 1");
		LogFile->write(EQEMuLog::Error, "Bazaar: Zero price transaction between %s and %s aborted."
						"Item: %s, Charges: %i, TBS: Qty %i, Price: %i",
						GetName(), Trader->GetName(),
						BuyItem->GetItem()->Name, BuyItem->GetCharges(), outtbs->Quantity, outtbs->Price);
		TradeRequestFailed(app);
		safe_delete(outapp);
		return;
	}

	//uint64 TotalTransactionValue = static_cast<uint64>(tbs->Price) * static_cast<uint64>(outtbs->Quantity);

	if(outtbs->Price > MAX_TRANSACTION_VALUE) {
		Message(13, "That would exceed the single transaction limit of %u platinum.", MAX_TRANSACTION_VALUE / 1000);
		TradeRequestFailed(app);
		safe_delete(outapp);
		return;
	}

	int TraderSlot = tbs->AlreadySold;
	ReturnTraderReq(app, outtbs->Quantity, TraderSlot, outtbs->Price);

	outtbs->TraderID = this->GetID();

	outtbs->Action = BazaarBuyItem;

	strn0cpy(outtbs->ItemName, BuyItem->GetItem()->Name, 64);

	int SlotID = 0;

	if(BuyItem->IsStackable())
		SendTraderItem(BuyItem->GetItem()->ID, outtbs->Quantity);
	else
		SendTraderItem(BuyItem->GetItem()->ID, BuyItem->GetCharges());

	EQApplicationPacket* outapp2 = new EQApplicationPacket(OP_MoneyUpdate,sizeof(MoneyUpdate_Struct));
	MoneyUpdate_Struct* mus= (MoneyUpdate_Struct*)outapp2->pBuffer;

	// This cannot overflow assuming MAX_TRANSACTION_VALUE, checked above, is the default of 2000000000
	uint32 TotalCost = outtbs->Price;
	this->TakeMoneyFromPP(TotalCost);

	mus->platinum = TotalCost / 1000;
	TotalCost -= (mus->platinum * 1000);
	mus->gold = TotalCost / 100;
	TotalCost -= (mus->gold * 100);
	mus->silver = TotalCost / 10;
	TotalCost -= (mus->silver * 10);
	mus->copper = TotalCost;

	bool updateclient = true;
	Trader->AddMoneyToPP(mus->copper,mus->silver,mus->gold,mus->platinum,updateclient);

	mus->platinum = Trader->GetPlatinum();
	mus->gold = Trader->GetGold();
	mus->silver = Trader->GetSilver();
	mus->copper = Trader->GetCopper();
	SlotID = Trader->FindTraderItem(tbs->ItemID, outtbs->Quantity);

	if(RuleB(Bazaar, AuditTrail))
		BazaarAuditTrail(Trader->GetName(), GetName(), BuyItem->GetItem()->Name, outtbs->Quantity, outtbs->Price, 0);

	Trader->FindAndNukeTraderItem(tbs->ItemID, outtbs->Quantity, this, SlotID);
	Trader->Trader_CustomerBought(this,TotalCost,tbs->ItemID,outtbs->Quantity,BuyItem->GetItem()->Name);

	Trader->QueuePacket(outapp);
	_pkt(TRADING__PACKETS, outapp);

	safe_delete(outapp);
	safe_delete(outapp2);

}

void Client::SendBazaarWelcome(){

	char errbuf[MYSQL_ERRMSG_SIZE];

	char* query = 0;

	MYSQL_RES *result;

	MYSQL_ROW row;

	if (database.RunQuery(query,MakeAnyLenString(&query, "select count(distinct char_id),count(char_id) from trader"),errbuf,&result)){
		if(mysql_num_rows(result)==1){

			row = mysql_fetch_row(result);

			EQApplicationPacket* outapp = new EQApplicationPacket(OP_BazaarSearch, sizeof(BazaarWelcome_Struct));

			memset(outapp->pBuffer,0,outapp->size);

			BazaarWelcome_Struct* bws = (BazaarWelcome_Struct*)outapp->pBuffer;

			bws->Beginning.Action = BazaarWelcome;

			bws->Items = atoi(row[1]);

			bws->Traders = atoi(row[0]);

			QueuePacket(outapp);

			safe_delete(outapp);
		}
		mysql_free_result(result);
	}
	safe_delete_array(query);

	if (database.RunQuery(query,MakeAnyLenString(&query, "select count(distinct charid) from buyer"),errbuf,&result)){
		if(mysql_num_rows(result)==1) {

			row = mysql_fetch_row(result);

		}
		mysql_free_result(result);
	}
	safe_delete_array(query);
}

void Client::SendBazaarResults(uint32 TraderID, uint32 Class_, uint32 Race, uint32 ItemStat, uint32 Slot, uint32 Type,
					char Name[64], uint32 MinPrice, uint32 MaxPrice) {

	char errbuf[MYSQL_ERRMSG_SIZE];
	char* Query = 0;
	std::string Search, Values;
	MYSQL_RES *Result;
	MYSQL_ROW Row;
	char Tmp[100] = {0};

	Values.append("count(item_id),trader.*,items.name");

	Search.append("where trader.item_id=items.id");

	if(TraderID > 0){
		Client* Trader = entity_list.GetClientByID(TraderID);

		if(Trader){
			sprintf(Tmp," and trader.char_id=%i",Trader->CharacterID());
			Search.append(Tmp);
		}

	}
	std::string SearchrResults;

	if(MinPrice != 0){
		sprintf(Tmp, " and trader.item_cost>=%i", MinPrice);
		Search.append(Tmp);
	}
	if(MaxPrice != 0){
		sprintf(Tmp, " and trader.item_cost<=%i", MaxPrice);
		Search.append(Tmp);
	}
	if(strlen(Name) > 0){
		char *SafeName = RemoveApostrophes(Name);
		sprintf(Tmp, " and items.name like '%%%s%%'", SafeName);
		safe_delete_array(SafeName);
		Search.append(Tmp);
	}
	if(Class_ != 0xFFFF){
		sprintf(Tmp, " and mid(reverse(bin(items.classes)),%i,1)=1", Class_);
		Search.append(Tmp);
	}
	if(Race!=0xFFFF){
		sprintf(Tmp, " and mid(reverse(bin(items.races)),%i,1)=1", Race);
		Search.append(Tmp);
	}
	if(Slot!=0xFFFF){
		sprintf(Tmp, " and mid(reverse(bin(items.slots)),%i,1)=1", Slot + 1);
		Search.append(Tmp);
	}
	if(Type!=0xFFFF){

		switch(Type){

			case 0:
				// 1H Slashing
				Search.append(" and items.itemtype=0 and damage>0");
				break;
			case 31:
				Search.append(" and items.itemclass=2");
				break;
			case 46:
				Search.append(" and items.spellid>0 and items.spellid<65000");
				break;
			case 47:
				Search.append(" and items.spellid=998");
				break;
			case 48:
				Search.append(" and items.spellid>=1298 and items.spellid<=1307");
				break;
			case 49:
				Search.append(" and items.focuseffect>0");
				break;
			default:
				sprintf(Tmp, " and items.itemtype=%i", Type);
				Search.append(Tmp);
		}
	}

	switch(ItemStat) {

		case STAT_AC:
			Search.append(" and items.ac>0");
			Values.append(",items.ac");
			break;

		case STAT_AGI:
			Search.append(" and items.aagi>0");
			Values.append(",items.aagi");
			break;

		case STAT_CHA:
			Search.append(" and items.acha>0");
			Values.append(",items.acha");
			break;

		case STAT_DEX:
			Search.append(" and items.adex>0");
			Values.append(",items.adex");
			break;

		case STAT_INT:
			Search.append(" and items.aint>0");
			Values.append(",items.aint");
			break;

		case STAT_STA:
			Search.append(" and items.asta>0");
			Values.append(",items.asta");
			break;

		case STAT_STR:
			Search.append(" and items.astr>0");
			Values.append(",items.astr");
			break;

		case STAT_WIS:
			Search.append(" and items.awis>0");
			Values.append(",items.awis");
			break;

		case STAT_COLD:
			Search.append(" and items.cr>0");
			Values.append(",items.cr");
			break;

		case STAT_DISEASE:
			Search.append(" and items.dr>0");
			Values.append(",items.dr");
			break;

		case STAT_FIRE:
			Search.append(" and items.fr>0");
			Values.append(",items.fr");
			break;

		case STAT_MAGIC:
			Values.append(",items.mr");
			Search.append(" and items.mr>0");
			break;

		case STAT_POISON:
			Search.append(" and items.pr>0");
			Values.append(",items.pr");
			break;

		case STAT_HP:
			Search.append(" and items.hp>0");
			Values.append(",items.hp");
			break;

		case STAT_MANA:
			Search.append(" and items.mana>0");
			Values.append(",items.mana");
			break;

		case STAT_ENDURANCE:
			Search.append(" and items.endur>0");
			Values.append(",items.endur");
			break;

		case STAT_ATTACK:
			Search.append(" and items.attack>0");
			Values.append(",items.attack");
			break;

		case STAT_HP_REGEN:
			Search.append(" and items.regen>0");
			Values.append(",items.regen");
			break;

		case STAT_MANA_REGEN:
			Search.append(" and items.manaregen>0");
			Values.append(",items.manaregen");
			break;

		case STAT_HASTE:
			Search.append(" and items.haste>0");
			Values.append(",items.haste");
			break;

		case STAT_DAMAGE_SHIELD:
			Search.append(" and items.damageshield>00");
			Values.append(",items.damageshield");
			break;

		default:
			Values.append(",0");
			break;
	}

	Values.append(",sum(charges), items.stackable ");

	if (database.RunQuery(Query,MakeAnyLenString(&Query, "select %s from trader,items %s group by items.id,charges,char_id limit %i",
							Values.c_str(),Search.c_str(), RuleI(Bazaar, MaxSearchResults)),errbuf,&Result)){

		_log(TRADING__CLIENT, "SRCH: %s", Query);
		safe_delete_array(Query);

		int Size = 0;
		uint32 ID = 0;

		if(mysql_num_rows(Result) == static_cast<unsigned long>(RuleI(Bazaar, MaxSearchResults)))
			Message(15, "Your search reached the limit of %i results. Please narrow your search down by selecting more options.",
					RuleI(Bazaar, MaxSearchResults));

		if(mysql_num_rows(Result) == 0){
			EQApplicationPacket* outapp2 = new EQApplicationPacket(OP_BazaarSearch, sizeof(BazaarReturnDone_Struct));
			BazaarReturnDone_Struct* brds = (BazaarReturnDone_Struct*)outapp2->pBuffer;
			brds->TraderID = ID;
			brds->Type = BazaarSearchDone;
			brds->Unknown008 = 0xFFFFFFFF;
			brds->Unknown012 = 0xFFFFFFFF;
			brds->Unknown016 = 0xFFFFFFFF;
			this->QueuePacket(outapp2);
			_pkt(TRADING__PACKETS,outapp2);
			safe_delete(outapp2);
			mysql_free_result(Result);
			return;
		}
		Size = sizeof(OldBazaarSearchResults_Struct);
		uchar *buffer = new uchar[Size];

		OldBazaarSearchResults_Struct* bsrs = (OldBazaarSearchResults_Struct*)buffer;
		while ((Row = mysql_fetch_row(Result))) {
			memset(buffer, 0, Size);

			bsrs->Action = BazaarSearchResults;
			bsrs->NumItems = atoi(Row[0]);
			Client* Trader2=entity_list.GetClientByCharID(atoi(Row[1]));
			if(Trader2){
				bsrs->SellerID = Trader2->GetID();
			}
			else{
				_log(TRADING__CLIENT, "Unable to find trader: %i\n",atoi(Row[1]));
			}
			bsrs->ItemID = atoi(Row[2]);
			//SerialNumber is atoi(Row[3]);
			//Charges is Row[4]
			bsrs->Cost = atoi(Row[5]);
			//SlotID is Row[6]
			memcpy(bsrs->ItemName, Row[7], strlen(Row[7]));
			bsrs->ItemStat = atoi(Row[8]);
			//SumCharges is Row[9]
			//Stackable is Row[10]

			EQApplicationPacket* outapp = new EQApplicationPacket(OP_BazaarSearch, Size);
			memcpy(outapp->pBuffer, buffer, Size);

			this->QueuePacket(outapp);
			_pkt(TRADING__PACKETS,outapp);
			safe_delete(outapp);
		}
		mysql_free_result(Result);
		
		safe_delete_array(buffer);

		EQApplicationPacket* outapp2 = new EQApplicationPacket(OP_BazaarSearch, sizeof(BazaarReturnDone_Struct));
		BazaarReturnDone_Struct* brds = (BazaarReturnDone_Struct*)outapp2->pBuffer;

		brds->TraderID = ID;
		brds->Type = BazaarSearchDone;

		brds->Unknown008 = 0xFFFFFFFF;
		brds->Unknown012 = 0xFFFFFFFF;
		brds->Unknown016 = 0xFFFFFFFF;

		this->QueuePacket(outapp2);

		_pkt(TRADING__PACKETS,outapp2);
		safe_delete(outapp2);

	}
	else{
		_log(TRADING__CLIENT, "Failed to retrieve Bazaar Search!! %s %s\n", Query, errbuf);
		safe_delete_array(Query);
		return;
	}
}

static void UpdateTraderCustomerItemsAdded(uint32 SellerID, uint32 CustomerID, TraderCharges_Struct* gis, uint32 ItemID) {

	// Send Item packets to the customer to update the Merchant window with the
	// new items for sale, and give them a message in their chat window.

	Client* Customer = entity_list.GetClientByID(CustomerID);
	Client* Seller = entity_list.GetClientByID(SellerID);
	uint32 SellerCharID = Seller->CharacterID();

	if(!Customer) return;

	const Item_Struct *item = database.GetItem(ItemID);

	if(!item) return;

	ItemInst* inst = database.CreateItem(item);

	if(!inst) return;

	Customer->Message(13, "The Trader has put up %s for sale.", item->Name);

	for(int i = 0; i < 80; i++) {

		if(gis->ItemID[i] == ItemID) {

			inst->SetCharges(gis->Charges[i]);

			inst->SetPrice(gis->ItemCost[i]);

			inst->SetSerialNumber(gis->SerialNumber[i]);

			inst->SetMerchantSlot(gis->SerialNumber[i]);

			if(inst->IsStackable())
				inst->SetMerchantCount(gis->Charges[i]);

			_log(TRADING__CLIENT, "Sending price update for %s, Serial No. %i with %i charges",
							item->Name, gis->SerialNumber[i], gis->Charges[i]);

			Customer->BulkSendTraderInventory(SellerCharID);
		}
	}

	safe_delete(inst);
}

static void UpdateTraderCustomerPriceChanged(uint32 SellerID, uint32 CustomerID, TraderCharges_Struct* gis, uint32 ItemID, int32 Charges, uint32 NewPrice) {

	// Send ItemPackets to update the customer's Merchant window with the new price (or remove the item if
	// the new price is 0) and inform them with a chat message.

	Client* Customer = entity_list.GetClientByID(CustomerID);
	Client* Seller = entity_list.GetClientByID(SellerID);
	uint32 SellerCharID = Seller->CharacterID();

	if(!Customer) return;

	const Item_Struct *item = database.GetItem(ItemID);

	if(!item) return;

	if(NewPrice == 0) {

		// If the new price is 0, remove the item(s) from the window.
			EQApplicationPacket* outapp = new EQApplicationPacket(OP_ShopDelItem, sizeof(Merchant_DelItem_Struct));
			Merchant_DelItem_Struct* tdis = (Merchant_DelItem_Struct*)outapp->pBuffer;
			
			tdis->playerid = 0;
			tdis->npcid = Customer->GetID();
			tdis->type=65;
			Customer->Message(13, "The Trader has withdrawn the %s from sale.", item->Name);

			for(int i = 0; i < 80; i++) {
				if(gis->ItemID[i] == ItemID) {
					tdis->itemslot = i;
					_log(TRADING__CLIENT, "Telling customer to remove item %i with %i charges.",ItemID, Charges);
					_pkt(TRADING__PACKETS, outapp);

					Customer->QueuePacket(outapp);
				}
			}
			safe_delete(outapp);
			return;

	}
	_log(TRADING__CLIENT, "Sending price updates to customer %s", Customer->GetName());

	ItemInst* inst = database.CreateItem(item);

	if(!inst) return;

	if(Charges > 0)
		inst->SetCharges(Charges);

	inst->SetPrice(NewPrice);

	if(inst->IsStackable())
		inst->SetMerchantCount(Charges);

	// Let the customer know the price in the window has suddenly just changed on them.
	Customer->Message(13, "The Trader has changed the price of %s.", item->Name);

	for(int i = 0; i < 80; i++) {
		if((gis->ItemID[i] != ItemID) ||
			((!item->Stackable) && (gis->Charges[i] != Charges)))
			continue;

		inst->SetSerialNumber(gis->SerialNumber[i]);

		inst->SetMerchantSlot(gis->SerialNumber[i]);

		_log(TRADING__CLIENT, "Sending price update for %s, Serial No. %i with %i charges",
						item->Name, gis->SerialNumber[i], gis->Charges[i]);

		Customer->BulkSendTraderInventory(SellerCharID);
	}
	safe_delete(inst);
}

void Client::HandleTraderPriceUpdate(const EQApplicationPacket *app) {

	// Handle price updates from the Trader and update a customer browsing our stuff if necessary
	// This method also handles removing items from sale and adding them back up whilst still in
	// Trader mode.
	//
	TraderPriceUpdate_Struct* tpus = (TraderPriceUpdate_Struct*)app->pBuffer;

	EQApplicationPacket *outapp = new EQApplicationPacket(OP_Trader, sizeof(Trader_ShowItems_Struct));
	Trader_ShowItems_Struct* tsis=(Trader_ShowItems_Struct*)outapp->pBuffer;

	tsis->TraderID = this->GetID();
	tsis->Code = tpus->Action;

	_log(TRADING__CLIENT, "Received Price Update for %s, Item Serial No. %i, New Price %i",
					GetName(), tpus->SerialNumber, tpus->NewPrice);

	// Pull the items this Trader currently has for sale from the trader table.
	//
	TraderCharges_Struct* gis = database.LoadTraderItemWithCharges(CharacterID());

	if(!gis) {
		_log(TRADING__CLIENT, "Error retrieving Trader items details to update price.");
		return;
	}

	// The client only sends a single update with the Serial Number of the item whose price has been updated.
	// We must update the price for all the Trader's items that are identical to that one item, i.e.
	// if it is a stackable item like arrows, update the price for all stacks. If it is not stackable, then
	// update the prices for all items that have the same number of charges.
	//
	uint32 IDOfItemToUpdate = 0;

	int32 ChargesOnItemToUpdate = 0;

	uint32 OldPrice = 0;

	uint32 SellerID = this->GetID();

	for(int i = 0; i < 80; i++) {

		if((gis->ItemID[i] > 0) && (gis->ItemID[i] == tpus->SerialNumber)) {
			// We found the item that the Trader wants to change the price of (or add back up for sale).
			//
			_log(TRADING__CLIENT, "ItemID is %i, Charges is %i", gis->ItemID[i], gis->Charges[i]);

			IDOfItemToUpdate = gis->ItemID[i];

			ChargesOnItemToUpdate = gis->Charges[i];

			OldPrice = gis->ItemCost[i];

			break;
		}
	}

	if(IDOfItemToUpdate == 0) {

		// If the item is not currently in the trader table for this Trader, then they must have removed it from sale while
		// still in Trader mode. Check if the item is in their Trader Satchels, and if so, put it back up.

		// Quick Sanity check. If the item is not currently up for sale, and the new price is zero, just ack the packet
		// and do nothing.
		if(tpus->NewPrice == 0) {
			tsis->SubAction = BazaarPriceChange_RemoveItem;
			_pkt(TRADING__PACKETS, outapp);
			QueuePacket(outapp);
			safe_delete(gis);
			return ;
		}

		_log(TRADING__CLIENT, "Unable to find item to update price for. Rechecking trader satchels. If you removed an item while in trader mode, this message can be ignored!");

		// Find what is in their Trader Satchels
		GetItems_Struct* newgis=GetTraderItems();

		uint32 IDOfItemToAdd = 0;

		int32 ChargesOnItemToAdd = 0;

		for(int i = 0; i < 80; i++) {

			if((newgis->Items[i] > 0) && (newgis->Items[i] == tpus->SerialNumber)) {

				_log(TRADING__CLIENT, "Found new Item to Add, ItemID is %i, Charges is %i", newgis->Items[i],
								newgis->Charges[i]);

				IDOfItemToAdd = newgis->Items[i];
				ChargesOnItemToAdd = newgis->Charges[i];

				break;
			}
		}


		const Item_Struct *item = 0;

		if(IDOfItemToAdd)
			item = database.GetItem(IDOfItemToAdd);

		if(!IDOfItemToAdd || !item) {

			_log(TRADING__CLIENT, "Item not found in Trader Satchels either.");
			tsis->SubAction = BazaarPriceChange_Fail;
			_pkt(TRADING__PACKETS, outapp);
			QueuePacket(outapp);
			Trader_EndTrader();
			safe_delete(gis);
			safe_delete(newgis);
			return;
		}

		// It is a limitation of the client that if you have multiple of the same item, but with different charges,
		// although you can set different prices for them before entering Trader mode. If you Remove them and then
		// add them back whilst still in Trader mode, they all go up for the same price. We check for this situation
		// and give the Trader a warning message.
		//
		if(!item->Stackable) {

			bool SameItemWithDifferingCharges = false;

			for(int i = 0; i < 80; i++) {
				if((newgis->Items[i] == IDOfItemToAdd) && (newgis->Charges[i] != ChargesOnItemToAdd)) {

					SameItemWithDifferingCharges = true;
					break;
				}
			}

			if(SameItemWithDifferingCharges)
				Message(13, "Warning: You have more than one %s with different charges. They have all been added for sale "
						"at the same price.", item->Name);
		}

		// Now put all Items with a matching ItemID up for trade.
		//
		for(int i = 0; i < 80; i++) {

			if(newgis->Items[i] == IDOfItemToAdd) {

				database.SaveTraderItem(CharacterID(), newgis->Items[i], newgis->SerialNumber[i], newgis->Charges[i],
							tpus->NewPrice, i);

				gis->ItemID[i] = newgis->Items[i];
				gis->Charges[i] = newgis->Charges[i];
				gis->SerialNumber[i] = newgis->SerialNumber[i];
				gis->ItemCost[i] = tpus->NewPrice;

				_log(TRADING__CLIENT, "Adding new item for %s. ItemID %i, SerialNumber %i, Charges %i, Price: %i, Slot %i",
							GetName(), newgis->Items[i], newgis->SerialNumber[i], newgis->Charges[i],
							tpus->NewPrice, i);
			}
		}

		// If we have a customer currently browsing, update them with the new items.
		//
		if(CustomerID)
			UpdateTraderCustomerItemsAdded(SellerID, CustomerID, gis, IDOfItemToAdd);

		safe_delete(gis);
		safe_delete(newgis);

		// Acknowledge to the client.
		tsis->SubAction = BazaarPriceChange_AddItem;
		_pkt(TRADING__PACKETS, outapp);
		QueuePacket(outapp);

		return;
	}

	// This is a safeguard against a Trader increasing the price of an item while a customer is browsing and
	// unwittingly buying it at a higher price than they were expecting to.
	//
	if((OldPrice != 0) && (tpus->NewPrice > OldPrice) && CustomerID) {

		tsis->SubAction = BazaarPriceChange_Fail;
		QueuePacket(outapp);
		Trader_EndTrader();
		Message(13, "You must remove the item from sale before you can increase the price while a customer is browsing.");
		Message(13, "Click 'Begin Trader' to restart Trader mode with the increased price for this item.");
		safe_delete(gis);
		return;
	}

	// Send Acknowledgement back to the client.
	if(OldPrice == 0)
		tsis->SubAction = BazaarPriceChange_AddItem;
	else if(tpus->NewPrice != 0)
		tsis->SubAction = BazaarPriceChange_UpdatePrice;
	else
		tsis->SubAction = BazaarPriceChange_RemoveItem;

	_pkt(TRADING__PACKETS, outapp);
	QueuePacket(outapp);

	if(OldPrice == tpus->NewPrice) {
		_log(TRADING__CLIENT, "The new price is the same as the old one.");
		safe_delete(gis);
		return;
	}
	// Update the price for all items we have for sale that have this ItemID and number of charges, or remove
	// them from the trader table if the new price is zero.
	//
	database.UpdateTraderItemPrice(CharacterID(), IDOfItemToUpdate, ChargesOnItemToUpdate, tpus->NewPrice);

	// If a customer is browsing our goods, send them the updated prices / remove the items from the Merchant window
	if(CustomerID)
		UpdateTraderCustomerPriceChanged(SellerID, CustomerID, gis, IDOfItemToUpdate, ChargesOnItemToUpdate, tpus->NewPrice);

	safe_delete(gis);

}
