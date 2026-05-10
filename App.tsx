import React, { useEffect, useRef, useState } from "react";

type Page = "home" | "motor" | "setup" | "calibrate" | "game" | "play" | "reminder";
type GameId = "blackjack" | "poker" | "texas_holdem" | "cambio" | "custom";

type LogEntry = {
  id: number;
  ts: string;
  level: "info" | "success" | "error";
  message: string;
};

type StatusResponse = {
  connected?: boolean;
  busy?: boolean;
  lastCommand?: string;
  lastResponse?: string;
  pairedButtons?: number;
  ejectReady?: boolean;
  players?: number;
  deckCount?: number;
  cardsPerDeck?: number;
  totalCards?: number;
  ejectPositionsSet?: number;
  ejectPositions?: Array<number | null>;
  motorPositions?: number[];
  buttonPololu2Percents?: number[];
  cambioStartupInProgress?: boolean;
  texasHoldemActive?: boolean;
  texasHoldemSequenceInProgress?: boolean;
  texasHoldemStage?: number;
  pokerActive?: boolean;
  pokerSequenceInProgress?: boolean;
  blackjackActive?: boolean;
  blackjackSequenceInProgress?: boolean;
};

const PLAYER_OPTIONS = [2, 3, 4, 5];
const DEFAULT_CARDS_PER_DECK = 52;

const GAMES: Array<{ id: GameId; label: string; description: string }> = [
  { id: "blackjack", label: "Blackjack", description: "Shuffle, deal 2 cards to each player, then buttons hit players." },
  { id: "poker", label: "Poker", description: "Shuffle, deal 5 cards to each player, then each player can draw up to 3 cards." },
  { id: "texas_holdem", label: "Texas Hold'em", description: "Shuffle, deal 2 hole cards each, then buttons advance flop, turn, river, and next hand." },
  { id: "cambio", label: "Cambio", description: "Shuffle and deal 4 cards to each player, then buttons draw one card." },
  { id: "custom", label: "Custom", description: "Shuffle first, then manually deal to any player from the web app or buttons." },
];

function startInstructions(game: GameId) {
  if (game === "cambio") {
    return "Press Start Game to queue a shuffle and deal 4 cards to each player. After that, each player presses their button to draw one card.";
  }
  if (game === "texas_holdem") {
    return "Press Start Game to queue a shuffle and deal 2 cards to each player. After that, any button press advances the table sequence: burn + flop, burn + turn, burn + river, then shuffle and start the next hand.";
  }
  if (game === "blackjack") {
    return "Press Start Game to queue a shuffle and deal 2 cards to each player. After that, players press their own button whenever they want another card.";
  }
  if (game === "poker") {
    return "Press Start Game to queue a shuffle and deal 5 cards to each player. After that, each player can press their own button to draw cards, but each player is limited to 3 draws.";
  }
  return "Press Start Game to shuffle the deck. After that, manually deal cards using the player buttons or the controls below.";
}

function nowStamp() {
  return new Date().toLocaleTimeString();
}

function normalizeUrl(url: string) {
  return url.trim().replace(/\/$/, "");
}

function buildCommandLine(command: string, args: Array<string | number> = []) {
  return [command, ...args].join(" ").trim();
}

function pause(ms: number) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function sectionCardStyle(): React.CSSProperties {
  return {
    background: "rgba(255, 255, 255, 0.96)",
    border: "1px solid rgba(212, 175, 55, 0.35)",
    borderRadius: 20,
    boxShadow: "0 10px 26px rgba(0, 0, 0, 0.18)",
  };
}

function buttonStyle(
  variant: "primary" | "secondary" | "danger" | "success" = "primary"
): React.CSSProperties {
  const base: React.CSSProperties = {
    border: "none",
    borderRadius: 14,
    padding: "14px 16px",
    fontSize: 15,
    fontWeight: 700,
    cursor: "pointer",
    transition: "0.15s ease",
  };

  if (variant === "danger") return { ...base, background: "#b91c1c", color: "white" };
  if (variant === "secondary") return { ...base, background: "#f4efe6", color: "#1f2937" };
  if (variant === "success") return { ...base, background: "#0f7a3f", color: "white" };

  return { ...base, background: "#991b1b", color: "white" };
}

function inputStyle(): React.CSSProperties {
  return {
    width: "100%",
    padding: "12px 14px",
    borderRadius: 14,
    border: "1px solid #cfd8e3",
    fontSize: 15,
    boxSizing: "border-box",
    background: "white",
    color: "#0f172a",
    caretColor: "#0f172a",
  };
}

function labelStyle(): React.CSSProperties {
  return {
    display: "block",
    marginBottom: 8,
    fontSize: 13,
    fontWeight: 700,
    color: "#334155",
  };
}

function pillStyle(active = false): React.CSSProperties {
  return {
    display: "inline-flex",
    alignItems: "center",
    justifyContent: "center",
    padding: "8px 12px",
    borderRadius: 999,
    background: active ? "#dcfce7" : "#eef2f7",
    color: active ? "#166534" : "#334155",
    fontWeight: 700,
    fontSize: 13,
  };
}

function cardTitle(title: string, subtitle?: string) {
  return (
    <div style={{ marginBottom: 18 }}>
      <div style={{ fontSize: 22, fontWeight: 800, marginBottom: 5 }}>{title}</div>
      {subtitle && <div style={{ color: "#64748b", lineHeight: 1.45 }}>{subtitle}</div>}
    </div>
  );
}

export default function App() {
  const [page, setPage] = useState<Page>("home");
  const [baseUrl, setBaseUrl] = useState("http://192.168.4.1");
  const [connected, setConnected] = useState(false);
  const [busy, setBusy] = useState(false);
  const [autoPoll, setAutoPoll] = useState(true);

  const [players, setPlayers] = useState(4);
  const [pendingPlayers, setPendingPlayers] = useState(4);
  const [game, setGame] = useState<GameId>("poker");
  const [gameStarted, setGameStarted] = useState(false);
  const [cambioInitialDealt, setCambioInitialDealt] = useState(false);
  const [texasHoldemStage, setTexasHoldemStage] = useState(0);
  const [cardsPerDeck, setCardsPerDeck] = useState(String(DEFAULT_CARDS_PER_DECK));

  const [pairedButtons, setPairedButtons] = useState(0);
  const [ejectReady, setEjectReady] = useState(false);
  const [lastCommand, setLastCommand] = useState("");
  const [lastResponse, setLastResponse] = useState("Not connected yet");
  const [motorPositions, setMotorPositions] = useState<number[]>([0, 0, 0]);
  const [seatPositions, setSeatPositions] = useState<Array<number | null>>(Array(8).fill(null));

  const [buttonPercents, setButtonPercents] = useState<number[]>(Array(8).fill(50));
  const [editingButtonPercents, setEditingButtonPercents] = useState<string[]>(Array(8).fill("50"));
  const [dirtyButtonPercents, setDirtyButtonPercents] = useState<boolean[]>(Array(8).fill(false));

  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [calibrationIndex, setCalibrationIndex] = useState(1);
  const [seatJogSteps, setSeatJogSteps] = useState("50");
  const previousPageRef = useRef<Page>(page);

  const [homeMotor, setHomeMotor] = useState("1");
  const [moveMotor, setMoveMotor] = useState("1");
  const [moveSteps, setMoveSteps] = useState("100");
  const [servoAngle, setServoAngle] = useState("90");
  const [spinMotor, setSpinMotor] = useState("1");
  const [spinSpeed, setSpinSpeed] = useState("200");
  const [spinMs, setSpinMs] = useState("1000");
  const [enableMotor, setEnableMotor] = useState("1");
  const [customCommand, setCustomCommand] = useState("");

  const [dealerOffset, setDealerOffset] = useState("150");
  const [communityOffset, setCommunityOffset] = useState("150");
  const [burnOffset, setBurnOffset] = useState("250");

  const activeSeatCount = Math.min(players, 8);
  const savedSeatCount = seatPositions.slice(0, activeSeatCount).filter((value) => value !== null).length;
  const basePosition = Number(motorPositions[2] ?? 0);
  const lastSeatPosition = Number(seatPositions[activeSeatCount - 1] ?? basePosition);

  const shellStyle: React.CSSProperties = {
    minHeight: "100vh",
    background:
      "radial-gradient(circle at center, #167a45 0%, #0f5f37 45%, #06351f 100%)",
    padding: 16,
    fontFamily: "Arial, Helvetica, sans-serif",
    color: "#0f172a",
  };

  const gridTwo: React.CSSProperties = {
    display: "grid",
    gridTemplateColumns: "repeat(auto-fit, minmax(260px, 1fr))",
    gap: 16,
  };

  function addLog(level: LogEntry["level"], message: string) {
    setLogs((prev) => [
      { id: Date.now() + Math.random(), ts: nowStamp(), level, message },
      ...prev,
    ].slice(0, 120));
  }

  async function readStatus(silent = false) {
    try {
      const res = await fetch(`${normalizeUrl(baseUrl)}/status`);
      const data: StatusResponse = await res.json();

      setConnected(true);
      setBusy(Boolean(data.busy));
      setPairedButtons(data.pairedButtons ?? 0);
      setEjectReady(Boolean(data.ejectReady));
      setLastCommand(data.lastCommand ?? "");
      setLastResponse(data.lastResponse ?? "Connected");
      if (typeof data.texasHoldemStage === "number") setTexasHoldemStage(data.texasHoldemStage);

      // Do NOT overwrite local player selection or cardsPerDeck from /status while using the phone UI.
      // Polling /status caused typed inputs and player choices to reset while the user was editing.

      if (Array.isArray(data.motorPositions)) {
        setMotorPositions([
          Number(data.motorPositions[0] ?? 0),
          Number(data.motorPositions[1] ?? 0),
          Number(data.motorPositions[2] ?? 0),
        ]);
      }

      if (Array.isArray(data.ejectPositions)) {
        const next = data.ejectPositions.slice(0, 8);
        while (next.length < 8) next.push(null);
        setSeatPositions(next);
      }

      if (Array.isArray(data.buttonPololu2Percents)) {
        const nextPercents = data.buttonPololu2Percents
          .slice(0, 8)
          .map((value) => Math.max(0, Math.min(100, Number(value ?? 50))));
        while (nextPercents.length < 8) nextPercents.push(50);

        // Keep saved-power display synced, but DO NOT overwrite the slider input here.
        // Polling /status while the user is dragging the slider made it snap back.
        setButtonPercents(nextPercents);
      }

      if (!silent) addLog("success", "Connected to ESP32 and fetched status.");
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setConnected(false);
      setBusy(false);
      setLastResponse(message);
      if (!silent) addLog("error", `Status request failed: ${message}`);
    }
  }

  async function sendCommand(command: string, silent = false) {
    const trimmed = command.trim();
    if (!trimmed) return false;

    setBusy(true);
    setLastCommand(trimmed);
    if (!silent) addLog("info", `Sent: ${trimmed}`);

    try {
      const res = await fetch(`${normalizeUrl(baseUrl)}/command`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ command: trimmed }),
      });
      const data = (await res.json()) as { ok?: boolean; message?: string };

      setConnected(true);
      setBusy(false);
      setLastResponse(data.message ?? "OK");
      addLog(res.ok && data.ok !== false ? "success" : "error", `${trimmed} → ${data.message ?? "No message"}`);
      await readStatus(true);
      return res.ok && data.ok !== false;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setConnected(false);
      setBusy(false);
      setLastResponse(message);
      addLog("error", `Command failed: ${message}`);
      return false;
    }
  }

  useEffect(() => {
    const previousPage = previousPageRef.current;
    previousPageRef.current = page;

    if (page === "calibrate" && previousPage !== "calibrate") {
      addLog("info", "Calibration opened: homing Motor 3 before player calibration.");
      void sendCommand("HOME 3");
    }
  }, [page]);

  async function confirmPlayers() {
    const count = Math.max(2, Math.min(5, Number(pendingPlayers) || 4));
    setPlayers(count);
    setPendingPlayers(count);
    setCalibrationIndex(1);
    setGameStarted(false);
    setCambioInitialDealt(false);
    setTexasHoldemStage(0);
    await sendCommand(`SETUP ${count} 1 ${Number(cardsPerDeck) || DEFAULT_CARDS_PER_DECK}`);
    await sendCommand("CLEARSEATS");
    setPage("calibrate");
  }

  async function shuffleWithCardCount() {
    const parsedCards = Number(cardsPerDeck);
    const safeCards = Number.isFinite(parsedCards) && parsedCards > 0 ? Math.round(parsedCards) : DEFAULT_CARDS_PER_DECK;
    setCardsPerDeck(String(safeCards));
    const setupOk = await sendCommand(`SETUP ${players} 1 ${safeCards}`);
    if (setupOk) await sendCommand("SHUFFLE");
  }


  async function startCambioGame() {
    const parsedCards = Number(cardsPerDeck);
    const safeCards = Number.isFinite(parsedCards) && parsedCards > 0 ? Math.round(parsedCards) : DEFAULT_CARDS_PER_DECK;
    setCardsPerDeck(String(safeCards));
    setGameStarted(false);
    setCambioInitialDealt(false);
    setTexasHoldemStage(0);

    addLog("info", "Cambio start: setting up, shuffling, then dealing 4 cards to each player.");

    const setupOk = await sendCommand(`SETUP ${players} 1 ${safeCards}`);
    if (!setupOk) return false;

    const cambioOk = await sendCommand("STARTCAMBIO");
    if (!cambioOk) return false;

    setGameStarted(true);
    setCambioInitialDealt(true);
    addLog("success", "Cambio ready. Shuffle complete and 4 cards dealt to each player. Buttons can now request more cards.");
    return true;
  }

  async function startTexasHoldemGame() {
    const parsedCards = Number(cardsPerDeck);
    const safeCards = Number.isFinite(parsedCards) && parsedCards > 0 ? Math.round(parsedCards) : DEFAULT_CARDS_PER_DECK;
    setCardsPerDeck(String(safeCards));
    setGameStarted(false);
    setTexasHoldemStage(0);

    addLog("info", "Texas Hold'em start: setting up, shuffling, then dealing 2 hole cards to each player.");

    const setupOk = await sendCommand(`SETUP ${players} 1 ${safeCards}`);
    if (!setupOk) return false;

    const holdemOk = await sendCommand("STARTTEXASHOLDEM");
    if (!holdemOk) return false;

    setGameStarted(true);
    setTexasHoldemStage(0);
    addLog("success", "Texas Hold'em ready. Hole cards are dealt. Button presses now advance flop, turn, river, then new hand.");
    return true;
  }

  async function startPokerGame() {
    const parsedCards = Number(cardsPerDeck);
    const safeCards = Number.isFinite(parsedCards) && parsedCards > 0 ? Math.round(parsedCards) : DEFAULT_CARDS_PER_DECK;
    setCardsPerDeck(String(safeCards));
    setGameStarted(false);
    setCambioInitialDealt(false);
    setTexasHoldemStage(0);

    addLog("info", "Poker start: setting up, shuffling, then dealing 5 cards to each player.");

    const setupOk = await sendCommand(`SETUP ${players} 1 ${safeCards}`);
    if (!setupOk) return false;

    const pokerOk = await sendCommand("STARTPOKER");
    if (!pokerOk) return false;

    setGameStarted(true);
    addLog("success", "Poker ready. Each player has 5 cards. Each player can now draw up to 3 cards with their button.");
    return true;
  }

  async function startBlackjackGame() {
    const parsedCards = Number(cardsPerDeck);
    const safeCards = Number.isFinite(parsedCards) && parsedCards > 0 ? Math.round(parsedCards) : DEFAULT_CARDS_PER_DECK;
    setCardsPerDeck(String(safeCards));
    setGameStarted(false);
    setCambioInitialDealt(false);
    setTexasHoldemStage(0);

    addLog("info", "Blackjack start: setting up, shuffling, then dealing 2 cards to each player.");

    const setupOk = await sendCommand(`SETUP ${players} 1 ${safeCards}`);
    if (!setupOk) return false;

    const blackjackOk = await sendCommand("STARTBLACKJACK");
    if (!blackjackOk) return false;

    setGameStarted(true);
    addLog("success", "Blackjack ready. Each player has 2 cards. Players can now press their button to hit.");
    return true;
  }

  async function jogBase(delta: number) {
    if (!Number.isFinite(delta) || delta === 0) return;
    await sendCommand(buildCommandLine("ENABLE", [3]), true);
    await sendCommand(buildCommandLine("MOVE", [3, delta]));
    await sendCommand(buildCommandLine("DISABLE", [3]), true);
  }

  async function homeBase() {
    await sendCommand("HOMEBASE");
  }

  async function clearSeats() {
    setCalibrationIndex(1);
    await sendCommand("CLEARSEATS");
  }

  async function saveCurrentSeat(buttonId: number) {
    // The updated firmware saves the current home-referenced base offset, then
    // automatically homes Motor 3 again before the app advances to the next player.
    const ok = await sendCommand(buildCommandLine("SETEJECTPOS", [buttonId]));
    if (ok && buttonId < activeSeatCount) setCalibrationIndex(buttonId + 1);
  }

  async function goToSeat(buttonId: number) {
    await sendCommand(buildCommandLine("GOTOSEAT", [buttonId]));
  }

  async function dealToSeat(buttonId: number) {
    // Firmware command does both parts: rotate to saved seat, then eject using that player's saved Pololu 2 power.
    // This is more reliable than sending GOTOSEAT and EJECT as two separate HTTP requests.
    const dealt = await sendCommand(buildCommandLine("DEALTOSEAT", [buttonId]));
    await pause(500);
    return dealt;
  }

  async function moveBaseToAbsolute(target: number) {
    const current = Number(motorPositions[2] ?? 0);
    const delta = Math.round(target - current);
    if (delta === 0) return true;
    await sendCommand(buildCommandLine("ENABLE", [3]), true);
    const ok = await sendCommand(buildCommandLine("MOVE", [3, delta]));
    await sendCommand(buildCommandLine("DISABLE", [3]), true);
    return ok;
  }

  async function dealToAbsolute(label: string, target: number) {
    addLog("info", `Deal to ${label} at ${Math.round(target)} steps`);
    const moved = await moveBaseToAbsolute(Math.round(target));
    if (!moved) return false;

    await pause(150);
    const ejected = await sendCommand("EJECT");
    await pause(250);
    return ejected;
  }

  async function dealRoundRobin(cardsEach: number) {
    for (let round = 0; round < cardsEach; round++) {
      addLog("info", `Starting deal round ${round + 1} of ${cardsEach}`);
      for (let p = 1; p <= activeSeatCount; p++) {
        const ok = await dealToSeat(p);
        if (!ok) {
          addLog("error", `Stopped deal sequence at Player ${p}, round ${round + 1}.`);
          return false;
        }
      }
    }
    return true;
  }

  async function dealBlackjackInitial() {
    const dealerTarget = lastSeatPosition + Number(dealerOffset || 0);
    for (let round = 0; round < 2; round++) {
      for (let p = 1; p <= activeSeatCount; p++) await dealToSeat(p);
      await dealToAbsolute("Dealer pile", dealerTarget);
    }
  }

  async function dealHoldemHoleCards() {
    await dealRoundRobin(2);
  }

  async function dealHoldemFlop() {
    const burnTarget = lastSeatPosition + Number(burnOffset || 0);
    const communityTarget = lastSeatPosition + Number(communityOffset || 0);
    await dealToAbsolute("Burn pile", burnTarget);
    await dealToAbsolute("Community pile", communityTarget);
    await dealToAbsolute("Community pile", communityTarget);
    await dealToAbsolute("Community pile", communityTarget);
  }

  async function dealHoldemTurnRiver(label: string) {
    const burnTarget = lastSeatPosition + Number(burnOffset || 0);
    const communityTarget = lastSeatPosition + Number(communityOffset || 0);
    await dealToAbsolute(`Burn pile before ${label}`, burnTarget);
    await dealToAbsolute(`Community pile ${label}`, communityTarget);
  }

  function clampedButtonPercent(buttonId: number) {
    const index = buttonId - 1;
    const raw = editingButtonPercents[index] ?? String(buttonPercents[index] ?? 50);
    const parsed = Number(raw);
    return Math.max(0, Math.min(100, Number.isFinite(parsed) ? parsed : 50));
  }

  function updateEditingButtonPercent(buttonId: number, value: string) {
    const index = buttonId - 1;
    setEditingButtonPercents((prev) => {
      const next = [...prev];
      next[index] = value;
      return next;
    });
    setDirtyButtonPercents((prev) => {
      const next = [...prev];
      next[index] = true;
      return next;
    });
  }

  async function saveButtonSpeed(buttonId: number) {
    const index = buttonId - 1;
    const percent = clampedButtonPercent(buttonId);
    const ok = await sendCommand(buildCommandLine("SETBUTTONSPEED", [buttonId, percent]));

    if (ok) {
      // Update local state immediately so the slider does not jump back while /status catches up.
      setButtonPercents((prev) => {
        const next = [...prev];
        next[index] = percent;
        return next;
      });
      setEditingButtonPercents((prev) => {
        const next = [...prev];
        next[index] = String(percent);
        return next;
      });
      setDirtyButtonPercents((prev) => {
        const next = [...prev];
        next[index] = false;
        return next;
      });
    }
  }

  async function testButtonSpeed(buttonId: number) {
    const index = buttonId - 1;
    const percent = clampedButtonPercent(buttonId);

    // Use the current slider value, then run a real card eject for that player.
    // This makes Test Power match the actual deal behavior instead of only spinning Pololu 2.
    const saved = await sendCommand(buildCommandLine("SETBUTTONSPEED", [buttonId, percent]));
    if (!saved) return;

    setButtonPercents((prev) => {
      const next = [...prev];
      next[index] = percent;
      return next;
    });
    setEditingButtonPercents((prev) => {
      const next = [...prev];
      next[index] = String(percent);
      return next;
    });

    await sendCommand(buildCommandLine("DEALTOSEAT", [buttonId]));
  }

  useEffect(() => {
    void readStatus(true);
  }, []);

  useEffect(() => {
    if (!autoPoll) return;
    const timer = setInterval(() => {
      void readStatus(true);
    }, 2500);
    return () => clearInterval(timer);
  }, [autoPoll, baseUrl]);

  function Header() {
    return (
      <div style={{ ...sectionCardStyle(), padding: 18 }}>
        <div style={{ display: "flex", justifyContent: "space-between", gap: 12, flexWrap: "wrap", alignItems: "center" }}>
          <div>
            <div style={{ fontSize: 26, fontWeight: 900 }}>♠ Card Dealer</div>
            <div style={{ color: "#64748b" }}>Phone control app for ESP32 motor commands and game dealing.</div>
          </div>
          <div style={pillStyle(connected)}>{connected ? "Connected" : "Disconnected"}</div>
        </div>
        <div style={{ display: "grid", gridTemplateColumns: "1fr auto", gap: 10, marginTop: 14 }}>
          <input style={inputStyle()} value={baseUrl} onChange={(e) => setBaseUrl(e.target.value)} />
          <button style={buttonStyle("secondary")} onClick={() => void readStatus(false)}>Test</button>
        </div>
        <div style={{ display: "flex", gap: 8, flexWrap: "wrap", marginTop: 12 }}>
          <span style={pillStyle(!busy)}>Busy: {busy ? "Yes" : "No"}</span>
          <span style={pillStyle(ejectReady)}>Eject Ready: {ejectReady ? "Yes" : "No"}</span>
          <span style={pillStyle(false)}>Base: {basePosition}</span>
          <span style={pillStyle(false)}>Cards/Deck: {cardsPerDeck}</span>
          <span style={pillStyle(false)}>Paired Buttons: {pairedButtons}</span>
        </div>
      </div>
    );
  }

  function HomePage() {
    return (
      <div style={gridTwo}>
        <div style={{ ...sectionCardStyle(), padding: 22 }}>
          {cardTitle("Motor Controls", "Direct machine commands: shuffle, eject, push, manual motion, servo, spin, enable, and disable.")}
          <button style={{ ...buttonStyle("success"), width: "100%" }} onClick={() => setPage("motor")}>Open Motor Controls</button>
        </div>
        <div style={{ ...sectionCardStyle(), padding: 22 }}>
          {cardTitle("Game Setup", "Choose players, calibrate every player seat, then select the game mode.")}
          <button style={{ ...buttonStyle("primary"), width: "100%" }} onClick={() => setPage("setup")}>Open Game Setup</button>
        </div>
      </div>
    );
  }

  function MotorPage() {
    return (
      <div style={{ display: "grid", gap: 16 }}>
        <div style={{ ...sectionCardStyle(), padding: 20 }}>
          {cardTitle("Motor Controls", "Enter cards per deck here, then Shuffle will update the ESP setup before running SHUFFLE.")}
          <button style={buttonStyle("secondary")} onClick={() => setPage("home")}>← Back Home</button>
          <div style={{ ...gridTwo, marginTop: 14 }}>
            <div>
              <label style={labelStyle()}>Cards per deck for shuffle</label>
              <input
                style={inputStyle()}
                type="number"
                min={1}
                value={cardsPerDeck}
                onChange={(e) => setCardsPerDeck(e.target.value)}
                placeholder="52"
              />
            </div>
            <button style={buttonStyle("primary")} onClick={() => void shuffleWithCardCount()}>Shuffle</button>
            <button style={buttonStyle("primary")} onClick={() => void sendCommand("EJECT")}>Eject</button>
            <button style={buttonStyle("secondary")} onClick={() => void sendCommand("PUSH")}>Push</button>
            <button style={buttonStyle("danger")} onClick={() => void sendCommand("ABORT")}>ABORT</button>
          </div>
        </div>

        <div style={{ ...sectionCardStyle(), padding: 20 }}>
          {cardTitle("Manual Motion", "Manual motion inputs. Enable/Disable lives here instead of a separate System Tools page.")}
          <div style={gridTwo}>
            <div>
              <label style={labelStyle()}>HOME motor</label>
              <select style={inputStyle()} value={homeMotor} onChange={(e) => setHomeMotor(e.target.value)}>
                <option value="1">Motor 1</option>
                <option value="2">Motor 2</option>
                <option value="3">Motor 3</option>
              </select>
              <button style={{ ...buttonStyle("primary"), width: "100%", marginTop: 10 }} onClick={() => void sendCommand(`HOME ${homeMotor}`)}>Send HOME</button>
            </div>

            <div>
              <label style={labelStyle()}>MOVE motor and steps</label>
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10 }}>
                <select style={inputStyle()} value={moveMotor} onChange={(e) => setMoveMotor(e.target.value)}>
                  <option value="1">Motor 1</option>
                  <option value="2">Motor 2</option>
                  <option value="3">Motor 3</option>
                </select>
                <input style={inputStyle()} value={moveSteps} onChange={(e) => setMoveSteps(e.target.value)} placeholder="-400" />
              </div>
              <button style={{ ...buttonStyle("primary"), width: "100%", marginTop: 10 }} onClick={() => void sendCommand(`MOVE ${moveMotor} ${moveSteps}`)}>Send MOVE</button>
            </div>

            <div>
              <label style={labelStyle()}>ENABLE / DISABLE motor</label>
              <select style={inputStyle()} value={enableMotor} onChange={(e) => setEnableMotor(e.target.value)}>
                <option value="1">Motor 1</option>
                <option value="2">Motor 2</option>
                <option value="3">Motor 3</option>
              </select>
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10, marginTop: 10 }}>
                <button style={buttonStyle("secondary")} onClick={() => void sendCommand(`ENABLE ${enableMotor}`)}>Enable</button>
                <button style={buttonStyle("secondary")} onClick={() => void sendCommand(`DISABLE ${enableMotor}`)}>Disable</button>
              </div>
            </div>

            <div>
              <label style={labelStyle()}>SERVO angle</label>
              <input style={inputStyle()} value={servoAngle} onChange={(e) => setServoAngle(e.target.value)} placeholder="90" />
              <button style={{ ...buttonStyle("primary"), width: "100%", marginTop: 10 }} onClick={() => void sendCommand(`SERVO ${servoAngle}`)}>Send SERVO</button>
            </div>

            <div>
              <label style={labelStyle()}>SPIN motor, speed, ms</label>
              <div style={{ display: "grid", gridTemplateColumns: "0.8fr 1fr 1fr", gap: 10 }}>
                <select style={inputStyle()} value={spinMotor} onChange={(e) => setSpinMotor(e.target.value)}>
                  <option value="1">P1</option>
                  <option value="2">P2</option>
                </select>
                <input style={inputStyle()} value={spinSpeed} onChange={(e) => setSpinSpeed(e.target.value)} placeholder="200" />
                <input style={inputStyle()} value={spinMs} onChange={(e) => setSpinMs(e.target.value)} placeholder="1000" />
              </div>
              <button style={{ ...buttonStyle("primary"), width: "100%", marginTop: 10 }} onClick={() => void sendCommand(`SPIN ${spinMotor} ${spinSpeed} ${spinMs}`)}>Send SPIN</button>
            </div>

            <div>
              <label style={labelStyle()}>Custom command</label>
              <input style={inputStyle()} value={customCommand} onChange={(e) => setCustomCommand(e.target.value)} placeholder="MOVE 2 -400" />
              <button style={{ ...buttonStyle("primary"), width: "100%", marginTop: 10 }} onClick={() => void sendCommand(customCommand)}>Send Command</button>
            </div>
          </div>
        </div>
      </div>
    );
  }

  function SetupPage() {
    return (
      <div style={{ ...sectionCardStyle(), padding: 20 }}>
        {cardTitle("Choose Number of Players", "Pick the number of players, then press Confirm Players. Next, you will calibrate each player position before choosing the game mode.")}
        <button style={buttonStyle("secondary")} onClick={() => setPage("home")}>← Back Home</button>

        <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit, minmax(150px, 1fr))", gap: 12, marginTop: 16 }}>
          {PLAYER_OPTIONS.map((count) => (
            <button key={count} style={buttonStyle(count === pendingPlayers ? "success" : "secondary")} onClick={() => setPendingPlayers(count)}>
              {count} Players
            </button>
          ))}
        </div>

        <div style={{ marginTop: 18, display: "grid", gap: 10 }}>
          <div style={pillStyle(false)}>Selected: {pendingPlayers} players</div>
          <button style={{ ...buttonStyle("primary"), width: "100%" }} onClick={() => void confirmPlayers()}>
            Confirm Players + Start Calibration
          </button>
        </div>
      </div>
    );
  }

  function CalibrationPage() {
    const currentSeat = Math.min(calibrationIndex, activeSeatCount);
    return (
      <div style={{ display: "grid", gap: 16 }}>
        <div style={{ ...sectionCardStyle(), padding: 20 }}>
          {cardTitle("Calibrate Players", `Currently calibrating Player ${currentSeat} of ${activeSeatCount}. Motor 3 homes when this page opens and after each saved player.`)}
          <button style={buttonStyle("secondary")} onClick={() => setPage("setup")}>← Back to Player Count</button>
          <div style={{ display: "flex", gap: 8, flexWrap: "wrap", margin: "14px 0" }}>
            <span style={pillStyle(false)}>Home-referenced base position: {basePosition}</span>
            <span style={pillStyle(savedSeatCount === activeSeatCount)}>Saved: {savedSeatCount}/{activeSeatCount}</span>
          </div>

          <div style={gridTwo}>
            <div>
              <label style={labelStyle()}>Jog steps</label>
              <input style={inputStyle()} value={seatJogSteps} onChange={(e) => setSeatJogSteps(e.target.value)} />
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10, marginTop: 10 }}>
                <button style={buttonStyle("secondary")} onClick={() => void jogBase(-Math.abs(Number(seatJogSteps) || 0))}>Jog -</button>
                <button style={buttonStyle("secondary")} onClick={() => void jogBase(Math.abs(Number(seatJogSteps) || 0))}>Jog +</button>
              </div>
            </div>
            <div>
              <label style={labelStyle()}>Current calibration player</label>
              <select style={inputStyle()} value={currentSeat} onChange={(e) => setCalibrationIndex(Number(e.target.value))}>
                {Array.from({ length: activeSeatCount }, (_, i) => (
                  <option key={i + 1} value={i + 1}>Player {i + 1}</option>
                ))}
              </select>
              <button style={{ ...buttonStyle("success"), width: "100%", marginTop: 10 }} onClick={() => void saveCurrentSeat(currentSeat)}>Save Current Position</button>
            </div>
          </div>

          <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit, minmax(150px, 1fr))", gap: 10, marginTop: 14 }}>
            <button style={buttonStyle("secondary")} onClick={() => void homeBase()}>Home Base</button>
            <button style={buttonStyle("danger")} onClick={() => void clearSeats()}>Clear Seats</button>
            <button style={buttonStyle("primary")} disabled={savedSeatCount < activeSeatCount} onClick={() => setPage("game")}>Continue to Game Mode</button>
          </div>
        </div>

        <div style={{ ...sectionCardStyle(), padding: 20 }}>
          {cardTitle("Player Positions + Pololu 2 Power", "Save each player home-referenced base position, then tune the Pololu 2 power slider for that player's deal/eject behavior. After each save, the firmware re-homes Motor 3 before you calibrate the next player.")}
          <div style={{ display: "grid", gap: 12 }}>
            {Array.from({ length: activeSeatCount }, (_, i) => {
              const id = i + 1;
              const saved = seatPositions[i];
              const editingPercent = editingButtonPercents[i] ?? "50";
              const savedPercent = buttonPercents[i] ?? 50;
              return (
                <div key={id} style={{ border: "1px solid #e2e8f0", borderRadius: 18, padding: 14, background: "#fbfdff", display: "grid", gap: 12 }}>
                  <div style={{ display: "flex", justifyContent: "space-between", gap: 10, flexWrap: "wrap" }}>
                    <div>
                      <div style={{ fontWeight: 900 }}>Player {id}</div>
                      <div style={{ color: "#64748b", fontSize: 13 }}>Saved position: {saved ?? "not saved"}</div>
                    </div>
                    <div style={{ display: "flex", gap: 8, flexWrap: "wrap" }}>
                      <button style={buttonStyle("secondary")} onClick={() => void goToSeat(id)}>Go To</button>
                      <button style={buttonStyle("primary")} onClick={() => void saveCurrentSeat(id)}>Save Position</button>
                    </div>
                  </div>

                  <div style={{ display: "grid", gridTemplateColumns: "1fr 80px", gap: 12, alignItems: "center" }}>
                    <input
                      type="range"
                      min={0}
                      max={100}
                      value={editingPercent}
                      onChange={(e) => updateEditingButtonPercent(id, e.target.value)}
                      style={{ width: "100%", accentColor: "#0f172a" }}
                    />
                    <input
                      style={{ ...inputStyle(), padding: "8px 10px" }}
                      type="number"
                      min={0}
                      max={100}
                      value={editingPercent}
                      onChange={(e) => updateEditingButtonPercent(id, e.target.value)}
                    />
                  </div>

                  <div style={{ display: "flex", gap: 10, flexWrap: "wrap", alignItems: "center" }}>
                    <span style={pillStyle(false)}>Saved power: {savedPercent}%</span>
                    <button style={buttonStyle("secondary")} onClick={() => void testButtonSpeed(id)}>Test Eject</button>
                    <button style={buttonStyle("success")} onClick={() => void saveButtonSpeed(id)}>Save Power</button>
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      </div>
    );
  }

  function GameSelectPage() {
    return (
      <div style={{ ...sectionCardStyle(), padding: 20 }}>
        {cardTitle("Select Game Mode", "Calibration is complete. Now choose what type of dealing sequence you want.")}
        <button style={buttonStyle("secondary")} onClick={() => setPage("calibrate")}>← Back to Calibration</button>
        <div style={{ display: "grid", gap: 12, marginTop: 14 }}>
          {GAMES.map((item) => (
            <button key={item.id} style={{ ...buttonStyle(item.id === game ? "success" : "secondary"), textAlign: "left" }} onClick={() => { setGame(item.id); setGameStarted(false); setCambioInitialDealt(false);
    setTexasHoldemStage(0); setPage("play"); }}>
              <div style={{ fontSize: 17 }}>{item.label}</div>
              <div style={{ fontSize: 13, opacity: 0.85 }}>{item.description}</div>
            </button>
          ))}
        </div>
      </div>
    );
  }

  function PlayPage() {
    return (
      <div style={{ display: "grid", gap: 16 }}>
        <div style={{ ...sectionCardStyle(), padding: 20 }}>
          {cardTitle(`Play: ${GAMES.find((g) => g.id === game)?.label ?? game}`, "These buttons run actual ESP commands: DEALTOSEAT for players, plus MOVE/EJECT for community or dealer pile offsets.")}
          <button
            style={buttonStyle("danger")}
            onClick={async () => {
              await sendCommand("ENDGAME", true);
              setGameStarted(false);
              setTexasHoldemStage(0);
              setCambioInitialDealt(false);
              setPage("reminder");
              addLog("info", "Game ended. Reminder shown before returning to game modes.");
              window.setTimeout(() => {
                setPage("game");
              }, 5000);
            }}
          >
            End Game
          </button>
          <div style={{ height: 12 }} />

          {!gameStarted && (
            <div style={{ display: "grid", gap: 12 }}>
              <div style={{ color: "#64748b", lineHeight: 1.45 }}>
                {startInstructions(game)}
              </div>
              <button
                style={{ ...buttonStyle("success"), width: "100%" }}
                onClick={async () => {
                  if (game === "cambio") {
                    await startCambioGame();
                    return;
                  }

                  if (game === "texas_holdem") {
                    await startTexasHoldemGame();
                    return;
                  }

                  if (game === "poker") {
                    await startPokerGame();
                    return;
                  }

                  if (game === "blackjack") {
                    await startBlackjackGame();
                    return;
                  }

                  const setupOk = await sendCommand(`SETUP ${players} 1 ${Number(cardsPerDeck) || DEFAULT_CARDS_PER_DECK}`);
                  if (setupOk) {
                    const shuffleOk = await sendCommand("SHUFFLE");
                    if (shuffleOk) {
                      await pause(800);
                      setGameStarted(true);
                    }
                  }
                }}
              >
                Start Game
              </button>
            </div>
          )}

          {gameStarted && game === "blackjack" && (
            <div style={{ display: "grid", gap: 12 }}>
              <div style={{ color: "#64748b", lineHeight: 1.45 }}>
                Blackjack is ready. The ESP32 shuffled and dealt 2 cards to each player. Players can now press their own button to hit, or you can deal manually below.
              </div>
              {Array.from({ length: activeSeatCount }, (_, i) => (
                <button key={i} style={buttonStyle("primary")} onClick={() => void dealToSeat(i + 1)}>Hit Player {i + 1}</button>
              ))}
            </div>
          )}

          {gameStarted && game === "poker" && (
            <div style={{ display: "grid", gap: 12 }}>
              <div style={{ color: "#64748b", lineHeight: 1.45 }}>
                Poker is ready. The ESP32 shuffled and dealt 5 cards to each player. Each player can press their own button to draw, but the firmware will only allow 3 draw cards per player.
              </div>
              {Array.from({ length: activeSeatCount }, (_, i) => (
                <button key={i} style={buttonStyle("primary")} onClick={() => void sendCommand(buildCommandLine("POKERDRAW", [i + 1]))}>Draw for Player {i + 1}</button>
              ))}
            </div>
          )}
          {gameStarted && game === "cambio" && (
            <div style={{ display: "grid", gap: 10 }}>
              <div style={{ color: "#64748b", lineHeight: 1.45 }}>
                Cambio is ready. The ESP32 shuffled and dealt 4 cards to each player. Players can now press their own button to draw one card, or you can deal manually below.
              </div>
              {Array.from({ length: activeSeatCount }, (_, i) => (
                <button key={i} style={buttonStyle("primary")} onClick={() => void dealToSeat(i + 1)}>Deal to Player {i + 1}</button>
              ))}
            </div>
          )}

          {gameStarted && game === "texas_holdem" && (
            <div style={{ display: "grid", gap: 12 }}>
              <div style={{ color: "#64748b", lineHeight: 1.45 }}>
                Texas Hold'em is running on the ESP32. The firmware shuffled and dealt 2 hole cards to each player.
                Any paired button now advances the table sequence: burn + flop, burn + turn, burn + river, then shuffle and start the next hand.
              </div>
              <div style={pillStyle(true)}>
                {texasHoldemStage === 0 && "Next button press: burn + flop"}
                {texasHoldemStage === 1 && "Next button press: burn + turn"}
                {texasHoldemStage === 2 && "Next button press: burn + river"}
                {texasHoldemStage >= 3 && "Next button press: shuffle + next hand"}
              </div>
              <button style={buttonStyle("secondary")} onClick={() => void sendCommand("TEXASHOLDEMADVANCE")}>
                Advance Texas Hold'em Manually
              </button>
            </div>
          )}

          {gameStarted && game === "custom" && (
            <div style={{ display: "grid", gap: 10 }}>
              {Array.from({ length: activeSeatCount }, (_, i) => (
                <button key={i} style={buttonStyle("primary")} onClick={() => void dealToSeat(i + 1)}>Deal to Player {i + 1}</button>
              ))}
            </div>
          )}
        </div>
      </div>
    );
  }


  function ReminderPage() {
    return (
      <div style={{ ...sectionCardStyle(), padding: 24, textAlign: "center" }}>
        <div style={{ fontSize: 26, fontWeight: 900, marginBottom: 12 }}>Game Ended</div>
        <div style={{ fontSize: 18, color: "#334155", lineHeight: 1.5 }}>
          Remember to put used cards back in the hopper.
        </div>
        <div style={{ marginTop: 18, color: "#64748b" }}>Returning to game modes...</div>
      </div>
    );
  }

  function StatusAndLogs() {
    return (
      <div style={gridTwo}>
        <div style={{ ...sectionCardStyle(), padding: 18 }}>
          {cardTitle("Live Status")}
          <div style={{ display: "grid", gap: 8, fontSize: 14 }}>
            <div><strong>Connected:</strong> {connected ? "Yes" : "No"}</div>
            <div><strong>Busy:</strong> {busy ? "Yes" : "No"}</div>
            <div><strong>Eject Ready:</strong> {ejectReady ? "Yes" : "No"}</div>
            <div><strong>Last Command:</strong> {lastCommand || "None"}</div>
            <div><strong>Last Response:</strong> {lastResponse || "None"}</div>
            <div><strong>Motor Positions:</strong> [{motorPositions.join(", ")}]</div>
            <div><strong>Cards per Deck:</strong> {cardsPerDeck}</div>
            <div><strong>Saved Seats:</strong> {savedSeatCount}/{activeSeatCount}</div>
          </div>
        </div>
        <div style={{ ...sectionCardStyle(), padding: 18 }}>
          {cardTitle("Event Log")}
          <div style={{ maxHeight: 260, overflow: "auto", display: "grid", gap: 8 }}>
            {logs.length === 0 ? <div style={{ color: "#64748b" }}>No events yet.</div> : logs.map((log) => (
              <div key={log.id} style={{ border: "1px solid #e2e8f0", borderRadius: 14, padding: 10, background: "#fbfdff" }}>
                <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 4, fontSize: 12, fontWeight: 800, color: log.level === "error" ? "#b91c1c" : log.level === "success" ? "#166534" : "#475569" }}>
                  <span>{log.level.toUpperCase()}</span><span>{log.ts}</span>
                </div>
                <div style={{ fontSize: 13 }}>{log.message}</div>
              </div>
            ))}
          </div>
        </div>
      </div>
    );
  }

  return (
    <div style={shellStyle}>
      <div style={{ maxWidth: 1100, margin: "0 auto", display: "grid", gap: 16 }}>
        {Header()}
        {page === "home" && (
          <>
            {HomePage()}
            <button style={buttonStyle("secondary")} onClick={() => setAutoPoll((v) => !v)}>{autoPoll ? "Stop Auto Poll" : "Start Auto Poll"}</button>
            {StatusAndLogs()}
          </>
        )}
        {page === "motor" && MotorPage()}
        {page === "setup" && SetupPage()}
        {page === "calibrate" && CalibrationPage()}
        {page === "game" && GameSelectPage()}
        {page === "play" && PlayPage()}
        {page === "reminder" && ReminderPage()}
      </div>
    </div>
  );
}
