-- Protection money. The hub's Coney script (level95_coney) has just loaded: the player is leaving
-- the hideout for the Coney streets.
--
-- What the game does: in the protection mission (level 86, chapter 1) the Warriors walk up to each
-- shop owner, press the action button ("Talk"), ask for the week's money, the owner hands it over
-- ($25, the money_take / money_give pair) and the Warrior reassures him. On the Coney streets
-- afterwards (shops.Setup) three of those owners stand in their stores with a 65 % chance each
-- (Stefano's butcher shop, Skinny Pete at Voodoo Motors, Twiggy at the Little Buddha) and carry $25
-- only while one of the game's three collection rounds is due; there the player can only mug them.
-- Charlie's plumbing store never opens there.
--
-- This mod, around the game's shops.Setup / shops.CleanUp:
--   * gives every open owner who carries money the mission's "Talk" prompt: press it and the
--     mission's own exchange plays (the Warrior's ask line, the owner's pay line, the money hand-over,
--     the reassuring line; Cleon's or Swan's lines, whoever is playing) and the player gets the $25
--     (mugging still works as in the game);
--   * an open owner who owes nothing to the game pays $25 again once the cooldown has passed (the
--     game's own collection rounds and their save flags stay as they are: a due round collected by
--     talking counts for the game exactly like a mugged one);
--   * opens Charlie too, with the same 65 % chance, on the pavement in front of his shutters, as a
--     fourth storekeeper of the same kind (GoalShopkeeper). He has no lines of his own for paying
--     (he pays in a cutscene in the mission), so he hands the money over without a word;
--   * opens two more payers the same way (same odds, same exchange, same cooldown): Hot Dog Jimmy
--     at his cart by Voodoo Motors (where the mission has him, a GoalShopkeeper in the hub's
--     ambient-sound box there) and the Gypsy on one of the hub's unused storekeeper flags
--     (fShopkeep[4]; no store box covers it, so she just stands there). Neither has a pay line;
--     Jimmy says one of his own mission lines when the Warriors hit him;
--   * a payment (talked or mugged) is noticed when the owner's money drops to 0; that store then waits
--     COOLDOWN ms of game time. The cooldowns and a downed extra payer live in this Lua state (the hub
--     session): after a mission or a reload every store starts fresh. Nothing but the player's money
--     (which the game saves itself) is written.

if not ProtectionMoney then
  ProtectionMoney = {
    CASH = 25,          -- the game's own protection payment
    OPEN = 35,          -- open when random(1, 100) > OPEN: the game's own 65 % roll
    COOLDOWN = 600000,  -- 10 minutes of game time between two payments of one store
    KEYS = {"butcher", "voodoo", "buddha"},  -- shops.keeper[1..3] of the game
    paid = {},          -- store key -> game time of its last payment (this hub session)
    open = {},          -- store key -> owner handle while the streets are up
    busy = {},          -- store key -> 1 while the hand-over plays
  }

  -- The mission's lines (level 86): the owner's pay line and, per Warrior, the ask and the
  -- reassurance. Cleon has the *_000 / *_003 lines, Swan (the mission's second player) *_004 /
  -- *_005. Charlie pays in a cutscene there and has none: Swan uses the game's unused name-free ask
  -- line t13_004 with him, Cleon only reassures him; another Warrior says nothing.
  local S = "vags/speeches/l86/"
  ProtectionMoney.LINES = {
    butcher = {give = S .. "l86_t3_001",
               cleon = {ask = S .. "l86_t3_000", calm = S .. "l86_t3_003"},
               swan = {ask = S .. "l86_t3_004", calm = S .. "l86_t3_005"}},
    voodoo = {give = S .. "l86_t4_001",
              cleon = {ask = S .. "l86_t4_000", calm = S .. "l86_t4_003"},
              swan = {ask = S .. "l86_t4_004", calm = S .. "l86_t4_005"}},
    buddha = {give = S .. "l86_t2_001",
              cleon = {ask = S .. "l86_t2_000", calm = S .. "l86_t2_003"},
              swan = {ask = S .. "l86_t2_004", calm = S .. "l86_t2_005"}},
    plumbing = {cleon = {calm = S .. "l86_t3_003"},
                swan = {ask = S .. "l86_t13_004", calm = S .. "l86_t3_005"}},
  }
  -- Jimmy and the Gypsy have no pay lines either: the Warriors use the same name-free lines with
  -- them as with Charlie.
  ProtectionMoney.LINES.hotdog = ProtectionMoney.LINES.plumbing
  ProtectionMoney.LINES.gypsy = ProtectionMoney.LINES.plumbing
  ProtectionMoney.WHO = {[2] = "cleon", [3] = "cleon", [189] = "cleon", [6] = "swan", [7] = "swan", [8] = "swan"}
  ProtectionMoney.ACTION_NEAR = 2     -- the hub's talk range for its own givers (CfgActionDistance)
  ProtectionMoney.ACTION_FAR = 1.1    -- and the range it puts back

  -- The payers the game never opens in the hub, in the order they are rolled. pos/heading: where
  -- they stand; box: the hub's ambient-sound box (vbAmbSound[box]) GoalShopkeeper keeps them in, or
  -- nil to just stand (a new AddVolumeBox while the streets are up crashes the game); hurt: their
  -- line the first time the Warriors hit them this session.
  --   * Charlie's store: in the hub its door is not placed and the shutters are down, so an open
  --     Charlie waits on the pavement in front of them, facing the street, in vbAmbSound[1]
  --     (-152.75..-123.25 x 40.45..57.75, over the store and that pavement).
  --   * Hot Dog Jimmy: the mission's spot at his cart (level 86 AddHotDogGuy), in vbAmbSound[4]
  --     (-96.5..-76.9 x 36.5..56.1, the Voodoo Motors corner); his line when hit there (l86_t10_006).
  --   * The Gypsy (character 312, configured but placed by no level): the hub's unused storekeeper
  --     flag fShopkeep[4]; no box covers it.
  ProtectionMoney.EXTRA = {
    {key = "plumbing", char = 303, name = "ShopkeeperPlumbing", pos = {-125, 44, 0.3}, heading = 180,
     box = 1, hurt = S .. "l86_l2_020", who = "Charlie"},
    {key = "hotdog", char = 315, name = "ShopkeeperHotDog", pos = {-91.7, 45.1, 0.3}, heading = 165,
     box = 4, hurt = S .. "l86_t10_006", who = "Jimmy"},
    {key = "gypsy", char = 312, name = "ShopkeeperGypsy", pos = {-169, 71.8, 0.3}, heading = 272,
     who = "the Gypsy"},
  }
  ProtectionMoney.gone = {}       -- store key -> 1 once its extra payer went down (this hub session)
  ProtectionMoney.complained = {} -- store key -> 1 once its extra payer said his hurt line

  ProtectionMoney.Due = function(key)
    local t = ProtectionMoney.paid[key]
    return t == nil or GetGameTime() - t >= ProtectionMoney.COOLDOWN
  end

  -- The Warrior's line for this store ("ask" or "calm"), or nil.
  ProtectionMoney.Line = function(key, p1, what)
    local who = ProtectionMoney.WHO[HuGetCharType(p1)]
    local set = who and ProtectionMoney.LINES[key][who]
    return set and set[what]
  end

  ProtectionMoney.KeyOf = function(h)
    for key, k in ProtectionMoney.open do
      if k == h then return key end
    end
    return nil
  end

  -- The "Talk" prompt on an owner who carries money. The storekeeper brain replaces an action
  -- handler set right after GoalShopkeeper, so the poll sets it again while he has money.
  ProtectionMoney.Offer = function(key, h)
    if HuIsAlive(h) and HuGetMoney(h) > 0 then
      SetMsgHandlerEx(h, 0, "ProtectionMoney.Ask", nil, GSTRING.PRESSTOTALK)
    end
  end

  -- Prompt pressed (p0 = the owner, p1 = who pressed): the Warrior asks, the money changes hands.
  ProtectionMoney.Ask = function(p0, p1)
    local PM = ProtectionMoney
    local key = PM.KeyOf(p0)
    if not key or PM.busy[key] or not (p1 == player or (player2 and p1 == player2)) then
      return nil
    end
    if HuGetMoney(p0) <= 0 then
      SetMsgHandler(p0, 0, nil)
      return nil
    end
    PM.busy[key] = 1
    PM.asker = p1
    SetMsgHandler(p0, 0, nil)
    HuBlockLook(p0, true)
    HuBlockLook(p1, true)
    BrDead(p0, true)
    HuUseAnim(p0, 0, nil)
    HuSetNormalMode(p1)
    ActLookAt(p0, p1, 0.1, -1)
    HuShutUp(p1)
    HuShutUp(p0)
    local ask = PM.Line(key, p1, "ask")
    if ask then
      HuSpeakNI(p1, ask, "ProtectionMoney.Give", p0)
      ScheduleFuncArg1("ProtectionMoney.HandOver", p0, 1500)
    else
      ScheduleFuncArg1("ProtectionMoney.HandOver", p0, 300)
      ScheduleFuncArg1("ProtectionMoney.Give", p0, 1300)
    end
    return true
  end

  ProtectionMoney.HandOver = function(p0)
    local PM = ProtectionMoney
    local key = PM.KeyOf(p0)
    local p1 = PM.asker
    if not key or not p1 or not HuIsAlive(p0) or HuGetMoney(p0) <= 0 then return end
    local cash = HuGetMoney(p0)
    HuSetMoney(p0, 0)
    PM.offered[key] = nil
    SetMsgHandler(p0, 0, nil)
    if p1 == player then
      GiveMoney(cash, 1)
    else
      GiveMoney(cash, 2)
    end
    if TestDistance(p1, p0, 1.3) then
      HuPlayDynPair(p1, p0, "money_take.anm", "money_give.anm")
    end
    PM.paid[key] = GetGameTime()
    ModLog(key .. " paid $" .. cash .. " (talk); player money " .. InvGetMoney(1))
  end

  -- The owner's pay line, then the Warrior's reassurance.
  ProtectionMoney.Give = function(p0)
    local PM = ProtectionMoney
    local key = PM.KeyOf(p0)
    if not key or not HuIsAlive(p0) then return end
    if PM.LINES[key].give then
      if PM.asker then ActLookAt(p0, PM.asker, 0.1, -1) end
      HuShutUp(p0)
      HuSpeakNI(p0, PM.LINES[key].give, "ProtectionMoney.Calm", p0)
    else
      ScheduleFuncArg1("ProtectionMoney.Calm", p0, 1500)
    end
  end

  ProtectionMoney.Calm = function(p0)
    local PM = ProtectionMoney
    local key = PM.KeyOf(p0)
    local p1 = PM.asker
    local calm = key and p1 and PM.Line(key, p1, "calm")
    if calm and HuIsAlive(p1) and HuIsAlive(p0) and TestDistance(p0, p1, 10) then
      HuSpeakNI(p1, calm, nil)
    end
    PM.Release(p0)
  end

  ProtectionMoney.Release = function(p0)
    local PM = ProtectionMoney
    local key = PM.KeyOf(p0)
    if PM.asker then HuBlockLook(PM.asker, false) end
    PM.asker = nil
    if key then PM.busy[key] = nil end
    if HuIsAlive(p0) then
      HuBlockLook(p0, false)
      BrDead(p0, false)
      if key and PM.boxes[key] then GoalShopkeeper(p0, PM.boxes[key], 4) end
    end
  end

  -- Twice a second while the streets are up: notice mugged owners, give an owner whose cooldown
  -- has passed his money (and the prompt) again, and, like the hub does for its own givers
  -- (roam.MiniGameGoodToGo), use the wider talk range while the player is near an owner who has
  -- something to pay, the hub's usual one otherwise.
  ProtectionMoney.Poll = function()
    local PM = ProtectionMoney
    if not PM.live then return end
    local near = nil
    for key, h in PM.open do
      if not HuIsAlive(h) then
        PM.open[key] = nil
      elseif not PM.busy[key] then
        if HuGetMoney(h) == 0 then
          if PM.offered[key] then
            PM.offered[key] = nil
            SetMsgHandler(h, 0, nil)
            PM.paid[key] = GetGameTime()
            ModLog(key .. " paid (mugged); player money " .. InvGetMoney(1))
          elseif PM.Due(key) then
            HuSetMoney(h, PM.CASH)
          end
        end
        if HuGetMoney(h) > 0 then
          PM.offered[key] = 1
          PM.Offer(key, h)
          if player and TestDistance(player, h, 4) then
            near = 1
          end
        end
      end
    end
    if near and not PM.wide then
      PM.wide = 1
      CfgActionDistance(1, PM.ACTION_NEAR)
    elseif PM.wide and not near then
      PM.wide = nil
      CfgActionDistance(1, PM.ACTION_FAR)
    end
    ScheduleFunc("ProtectionMoney.Poll", 500)
  end

  ProtectionMoney.ExtraDied = function(p0, p1)
    local e = ProtectionMoney.extraOf[p0]
    if e then
      ProtectionMoney.gone[e.key] = 1
      ModLog(e.key .. ": " .. e.who .. " is down, closed for this session")
    end
  end

  ProtectionMoney.ExtraHurt = function(p0, p1, p2)
    local PM = ProtectionMoney
    local e = PM.extraOf[p0]
    if e and e.hurt and not PM.complained[e.key] and HuIsAlive(p0) and not PM.busy[e.key] then
      PM.complained[e.key] = 1
      HuSpeakNI(p0, e.hurt, nil)
    end
  end

  -- One of the payers the game never opens here (an entry of ProtectionMoney.EXTRA): the game's own
  -- 65 % roll, then a storekeeper of the game's kind (gang 23, GoalShopkeeper when the hub has a box
  -- there) who owes the $25 unless his store is cooling down.
  ProtectionMoney.OpenExtra = function(e)
    local PM = ProtectionMoney
    if PM.gone[e.key] then
      ModLog(e.key .. ": closed (" .. e.who .. " went down earlier this session)")
      return
    end
    local box = nil
    if e.box then
      box = vbAmbSound and vbAmbSound[e.box]
      if not box then
        ModLog(e.key .. ": the hub has no store box here, not opened")
        return
      end
    end
    if random(1, 100) <= PM.OPEN then
      ModLog(e.key .. ": closed today")
      return
    end
    if not PM.gang then
      PM.gang = GangCreate(23, "Shopkeep_pm", 0, 0)
    end
    local h = HuCreate(e.name, e.char, e.pos, e.heading, "warr_sw", 0, PM.gang)
    Teleport(h, e.pos, e.heading)
    PM.extraOf[h] = e
    if box then
      PM.boxes[e.key] = box
      GoalShopkeeper(h, box, 4)
    end
    SetMsgHandler(h, 18, "ProtectionMoney.ExtraDied")
    SetMsgHandler(h, 1, "ProtectionMoney.ExtraHurt")
    HuSetMoney(h, 0)
    if PM.Due(e.key) then
      HuSetMoney(h, PM.CASH)
    end
    PM.open[e.key] = h
    ModLog(e.key .. ": " .. e.who .. " is open, money " .. HuGetMoney(h))
  end

  ProtectionMoney.Setup = function()
    local PM = ProtectionMoney
    local unlocked = UM_IsDataUnlocked(6, 2)
    if unlocked and roam and roam.Shopkeepers then
      -- Load the extra payers' models with the game's storekeepers (coney.SetUpModels loads and
      -- unloads every number in this list).
      for i = 1, getn(PM.EXTRA) do
        tinsert(roam.Shopkeepers, PM.EXTRA[i].char)
      end
    end
    PM.open = {}
    PM.busy = {}
    PM.offered = {}
    PM.boxes = {}
    PM.extraOf = {}
    PM.asker = nil
    PM.gang = nil
    PM.retailSetup()
    if not unlocked or not shops then
      return
    end
    SetDynamicAnimation("money_take.anm", false)
    SetDynamicAnimation("money_give.anm", false)
    PM.anims = 1
    for i = 1, 3 do
      local key = PM.KEYS[i]
      local k = shops.keeper[i]
      if k and k.gangTable and k.gangTable[1] and HuIsAlive(k.gangTable[1]) then
        local h = k.gangTable[1]
        local owes = "game round"
        if HuGetMoney(h) == 0 then
          if PM.Due(key) then
            HuSetMoney(h, PM.CASH)
            owes = "mod"
          else
            owes = "cooldown"
          end
        end
        PM.open[key] = h
        PM.boxes[key] = vbStore and vbStore[i]
        ModLog(key .. ": open, money " .. HuGetMoney(h) .. " (" .. owes .. ")")
      else
        ModLog(key .. ": closed today")
      end
    end
    for i = 1, getn(PM.EXTRA) do
      PM.OpenExtra(PM.EXTRA[i])
    end
    for key, h in PM.open do
      if HuGetMoney(h) > 0 then
        PM.offered[key] = 1
        PM.Offer(key, h)
      end
    end
    PM.live = 1
    PM.wide = nil
    ScheduleFunc("ProtectionMoney.Poll", 500)
  end

  ProtectionMoney.CleanUp = function()
    local PM = ProtectionMoney
    PM.live = nil
    if PM.wide then
      PM.wide = nil
      CfgActionDistance(1, PM.ACTION_FAR)
    end
    for key, h in PM.open do
      if HuIsAlive(h) then SetMsgHandler(h, 0, nil) end
    end
    PM.open = {}
    if PM.gang then
      GangClearHandlers(PM.gang)
      RemoveGang(PM.gang)
      PM.gang = nil
    end
    if PM.anims then
      SetDynamicAnimation("money_take.anm", true)
      SetDynamicAnimation("money_give.anm", true)
      PM.anims = nil
    end
    PM.retailCleanUp()
  end
end

-- The Coney script is loaded again each time the player leaves the hideout: wrap its new shops.
if shops and shops.Setup and shops.CleanUp then
  ProtectionMoney.retailSetup = shops.Setup
  ProtectionMoney.retailCleanUp = shops.CleanUp
  shops.Setup = ProtectionMoney.Setup
  shops.CleanUp = ProtectionMoney.CleanUp
  ModLog("wrapped the Coney storekeepers")
else
  ModLog("level95_coney has no shops table: nothing to do")
end
