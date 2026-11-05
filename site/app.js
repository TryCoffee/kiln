// Kiln's website: the download buttons point at the latest release's files,
// the visitor's system first; the install tabs open on that system too.
// Without JavaScript, or if GitHub's API does not answer, every link still
// goes to the latest release's page.
(function () {
  "use strict";

  const RELEASES = "https://github.com/TryCoffee/kiln/releases/latest";
  const API = "https://api.github.com/repos/TryCoffee/kiln/releases/latest";

  function detectOS() {
    const p = ((navigator.userAgentData && navigator.userAgentData.platform) ||
               navigator.platform || navigator.userAgent || "").toLowerCase();
    if (p.includes("win")) return "windows";
    if (p.includes("mac")) return "macos";
    if (p.includes("linux") && !/android/i.test(navigator.userAgent)) return "linux";
    return null;
  }

  // Which release file each button downloads (never makepkg's -debug package).
  const PICK = {
    windows: { label: "Download for Windows", match: /-x64-setup\.exe$/ },
    macos: { label: "Download for macOS", match: /-arm64\.dmg$/ },
    linux: { label: "Download for Linux", match: null },  // several packages: the release page
  };

  const os = detectOS();
  const primary = document.getElementById("dl-primary");
  const others = document.querySelectorAll("#dl-others a");

  function setTab(name) {
    document.querySelectorAll(".tabs button").forEach(function (b) {
      b.setAttribute("aria-selected", String(b.dataset.tab === name));
    });
    document.querySelectorAll(".panel").forEach(function (p) {
      p.hidden = p.dataset.panel !== name;
    });
  }
  document.querySelectorAll(".tabs button").forEach(function (b) {
    b.addEventListener("click", function () { setTab(b.dataset.tab); });
  });

  if (os) {
    primary.textContent = PICK[os].label;
    others.forEach(function (a) { if (a.dataset.os === os) a.hidden = true; });
    setTab(os);
  }

  fetch(API, { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (release) {
      const assets = release.assets || [];
      function urlFor(name) {
        const rule = PICK[name];
        if (!rule || !rule.match) return RELEASES;
        const a = assets.find(function (x) { return rule.match.test(x.name); });
        return a ? a.browser_download_url : RELEASES;
      }
      if (os) primary.href = urlFor(os);
      others.forEach(function (a) { a.href = urlFor(a.dataset.os); });
      const v = document.getElementById("dl-version");
      if (release.tag_name) {
        v.textContent = "Version " + release.tag_name.replace(/^v/, "") + " · " + v.textContent;
      }
    })
    .catch(function () { /* links already point at the release page */ });
})();
