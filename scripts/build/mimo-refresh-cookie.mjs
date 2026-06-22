#!/usr/bin/env node
/**
 * mimo-refresh-cookie.mjs — use Playwright to grab fresh MiMo session cookies
 * from an authenticated browser session, then store them in macOS Keychain.
 *
 * Flow:
 *   1. Launch Chromium with a persistent profile that holds the MiMo login.
 *   2. Navigate to the MiMo usage page (triggers cookie refresh).
 *   3. Extract userId, api-platform_ph, api-platform_slh from the cookie jar.
 *   4. Write the Cookie header string to Keychain via `security`.
 *
 * Usage:
 *   node mimo-refresh-cookie.mjs              # headless, uses saved profile
 *   node mimo-refresh-cookie.mjs --login      # headed, one-time login flow
 *   node mimo-refresh-cookie.mjs --check      # test if stored cookie still works
 *   node mimo-refresh-cookie.mjs --help
 *
 * The profile lives at ~/.config/codexbar-toy/mimo-chrome-profile/.
 * First run: `--login` opens a visible window for you to log in.
 * After that, headless refreshes work without user interaction.
 *
 * Keychain:
 *   service=codexbar-toy  account=mimo-session
 *   (same slot codexbar-publish.sh --set-mimo-cookie uses)
 */

import { chromium } from "playwright";
import { execSync } from "child_process";
import { existsSync, mkdirSync } from "fs";
import { homedir } from "os";
import { resolve } from "path";

const KC_SERVICE = "codexbar-toy";
const KC_ACCOUNT = "mimo-session";
const MIMO_URL = "https://platform.xiaomimimo.com/console/usage";
const PROFILE_DIR = resolve(homedir(), ".config", "codexbar-toy", "mimo-chrome-profile");

const args = process.argv.slice(2);
const mode = args[0] || "--refresh";

if (mode === "--help" || mode === "-h") {
  console.log(`mimo-refresh-cookie — grab fresh MiMo session cookies via Playwright

Usage:
  node mimo-refresh-cookie.mjs              Refresh cookies (headless)
  node mimo-refresh-cookie.mjs --login      One-time login (headed window)
  node mimo-refresh-cookie.mjs --check      Test if stored cookie is valid
  node mimo-refresh-cookie.mjs --help       This message

Profile: ${PROFILE_DIR}
Keychain: service=${KC_SERVICE} account=${KC_ACCOUNT}`);
  process.exit(0);
}

// ── Keychain helpers ────────────────────────────────────────────────────────

function keychainGet() {
  try {
    return execSync(
      `security find-generic-password -s "${KC_SERVICE}" -a "${KC_ACCOUNT}" -w`,
      { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"] }
    ).trim();
  } catch {
    return null;
  }
}

function keychainSet(value) {
  execSync(
    `security add-generic-password -U -s "${KC_SERVICE}" -a "${KC_ACCOUNT}" ` +
      `-l "CodexBar toy MiMo session" -w "${value}"`,
    { stdio: "pipe" }
  );
}

// ── Cookie extraction ───────────────────────────────────────────────────────

function buildCookieHeader(cookies) {
  // We need: userId, api-platform_ph, api-platform_slh, and api-platform_serviceToken
  // (HttpOnly — required for API auth but invisible to document.cookie)
  const needed = ["userId", "api-platform_ph", "api-platform_slh", "api-platform_serviceToken"];
  const found = [];
  for (const name of needed) {
    const c = cookies.find(
      (c) => c.name === name && c.domain.includes("xiaomimimo")
    );
    if (c) {
      // Strip quotes from values (Playwright returns them quoted)
      let val = c.value;
      if (val.startsWith('"') && val.endsWith('"')) val = val.slice(1, -1);
      found.push(`${c.name}=${val}`);
    } else {
      console.warn(`  ⚠ Missing cookie: ${name}`);
    }
  }
  return found.length >= 3 ? found.join("; ") : null;
}

// ── Cookie validation ───────────────────────────────────────────────────────

async function validateCookie(cookie) {
  // Quick probe: hit the balance endpoint
  const resp = await fetch("https://platform.xiaomimimo.com/api/v1/balance", {
    headers: {
      Cookie: cookie,
      Accept: "application/json",
      "User-Agent":
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
    },
  });
  if (resp.status === 401) return false;
  if (resp.ok) {
    const data = await resp.json();
    return data.code === 0;
  }
  return false;
}

// ── --check mode ────────────────────────────────────────────────────────────

if (mode === "--check") {
  const existing = keychainGet();
  if (!existing) {
    console.log("❌ No cookie in Keychain");
    process.exit(1);
  }
  const ok = await validateCookie(existing);
  console.log(ok ? "✅ Cookie is valid" : "❌ Cookie expired/invalid");
  process.exit(ok ? 0 : 1);
}

// ── Ensure profile directory exists ─────────────────────────────────────────

if (!existsSync(PROFILE_DIR)) {
  mkdirSync(PROFILE_DIR, { recursive: true });
}

// ── Launch browser ──────────────────────────────────────────────────────────

const headless = mode !== "--login";
console.log(
  headless
    ? "🔒 Launching headless Chromium (using saved profile)..."
    : "🌐 Launching Chromium (headed — log in to MiMo, then close the window)..."
);

const context = await chromium.launchPersistentContext(PROFILE_DIR, {
  headless,
  args: ["--disable-blink-features=AutomationControlled"],
  // Accept Google sign-in popups etc.
  acceptDownloads: false,
});

const page = context.pages()[0] || await context.newPage();

if (mode === "--login") {
  // Navigate to login page — user logs in manually
  console.log("📖 Opening MiMo console...");
  await page.goto("https://platform.xiaomimimo.com/console", {
    waitUntil: "domcontentloaded",
    timeout: 30000,
  });

  console.log("");
  console.log("╔══════════════════════════════════════════════════════════╗");
  console.log("║  Log in to MiMo in the browser window that just opened. ║");
  console.log("║  Once you can see the usage page, close the window.     ║");
  console.log("╚══════════════════════════════════════════════════════════╝");
  console.log("");

  // Wait for the window to be closed (user closes it after login)
  await page.waitForEvent("close", { timeout: 300_000 }).catch(() => {});
  // If the page didn't close (e.g. user navigated), just wait a bit
  await new Promise((r) => setTimeout(r, 1000));

  console.log("✅ Login session saved to profile. You can now run headless refreshes.");
  await context.close();
  process.exit(0);
}

// ── Refresh flow (headless) ─────────────────────────────────────────────────

try {
  console.log(`📖 Navigating to ${MIMO_URL}...`);
  await page.goto(MIMO_URL, {
    waitUntil: "load",
    timeout: 30_000,
  });

  // Wait for network to settle, then give the SPA time to set cookies
  await page.waitForLoadState("networkidle", { timeout: 20_000 }).catch(() => {});
  await new Promise((r) => setTimeout(r, 5000));

  // Extract cookies from the browser context (no URL filter — domain matching is
  // unreliable across redirects, so grab everything and filter by domain ourselves)
  const allCookies = await context.cookies();
  const mimoCookies = allCookies.filter(c => c.domain.includes("xiaomimimo"));
  console.log(`🍪 Found ${mimoCookies.length} cookies for xiaomimimo.com`);

  const cookieHeader = buildCookieHeader(mimoCookies);

  if (!cookieHeader) {
    console.error(
      "❌ Could not find required cookies (userId, api-platform_ph, api-platform_slh)."
    );
    console.error("   You may need to run --login first to authenticate.");
    const names = allCookies.map((c) => c.name).join(", ");
    console.error(`   Cookies found: ${names || "(none)"}`);
    await context.close();
    process.exit(1);
  }

  // Validate before storing
  console.log("🔍 Validating cookie...");
  const valid = await validateCookie(cookieHeader);

  if (!valid) {
    console.error("❌ Cookie is invalid or expired. Run --login to re-authenticate.");
    await context.close();
    process.exit(1);
  }

  // Store in Keychain
  keychainSet(cookieHeader);
  const stored = keychainGet();
  const bytes = Buffer.byteLength(stored || "", "utf-8");
  console.log(`✅ Cookie stored in Keychain (${bytes} bytes)`);
  console.log(`   service=${KC_SERVICE} account=${KC_ACCOUNT}`);

  // Quick balance check for confirmation
  const resp = await fetch("https://platform.xiaomimimo.com/api/v1/balance", {
    headers: {
      Cookie: cookieHeader,
      Accept: "application/json",
      "User-Agent":
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
    },
  });
  if (resp.ok) {
    const data = await resp.json();
    if (data.code === 0) {
      const d = data.data;
      console.log(
        `💰 Balance: $${d.cashBalance} cash, $${d.giftBalance} gift`
      );
    }
  }
} catch (err) {
  console.error(`❌ Error: ${err.message}`);
  if (err.message.includes("ersistent context is already")) {
    console.error(
      "   Another Chromium instance may be using this profile. Close it first."
    );
  }
  process.exit(1);
} finally {
  await context.close();
}
