#!/bin/bash
cd "$(dirname "$0")/packages"
crx() { # name id
  [ -s "$1.crx" ] && return
  curl -sSL -o "$1.crx" -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36" "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=130.0&acceptformat=crx2,crx3&x=id%3D$2%26uc"
  echo "$1 $(stat -c %s $1.crx)"
}
xpi() { # name slug
  [ -s "$1.xpi" ] && return
  curl -sSL -o "$1.xpi" "https://addons.mozilla.org/firefox/downloads/latest/$2/latest.xpi"
  echo "$1 $(stat -c %s $1.xpi)"
}
crx bitwarden-chrome nngceckbapebfimnlniiiahkandclblb
crx darkreader-chrome eimadpbcbfnmbkopoojfekhnkhdbieeh
crx google-translate aapbdbdomjkkjkaonfhkkikfgjllcleb
crx grammarly kbfnbcaeplbcioakkpcpgfkobkghlhen
crx return-youtube-dislike gebbhagfogifgggkldgodflihgfeippi
crx sponsorblock mnjggcdmjocbbbhaepdhchncahnbgone
crx vimium dbepggeogbaibhgnhhndojpepiihcmeb
crx react-devtools fmkadmapgofadopljbjfkapdkoienihi
crx json-formatter bcjindcccaagfpapjjmafapmmgkkhgoa
crx ublock-lite ddkjiahejlhfcafbddmgiahcphecmpfh
crx adguard bgnkhhnnamicmpeenaelnjfhikgbkllg
crx ghostery mlomiejdfkolichcflejclcbmpeaniij
crx tampermonkey dhdgffkkebhmkfjojejmpbldmpobfkfo
crx violentmonkey jinjaccalgkegednnccohejagnlnfdag
crx honey bmnlcjabgnpnenekpadlanbbkooimhnj
crx keepa neebplgakaahbhdphmkckjjcegoiijjo
crx proton-pass ghmbeldphafepmbegfdlkpapadhbakde
crx lastpass hdokiejnpimakedhajhdlcegeplioahd
crx momentum laookkfknpbbblfpciffpaejjkokdgca
crx onetab chphlpgkkbolifaimnlloiipkdnihall
crx stylus-chrome clngdbkpkpeebahjckkjfobafhncgmne
xpi bitwarden-firefox bitwarden-password-manager
xpi privacy-badger privacy-badger17
xpi multi-account-containers multi-account-containers
xpi tree-style-tab tree-style-tab
xpi stylus-firefox styl-us
xpi clearurls clearurls
xpi cookie-autodelete cookie-autodelete
xpi decentraleyes decentraleyes
xpi localcdn localcdn-fork-of-decentraleyes
xpi violentmonkey-firefox violentmonkey
xpi sponsorblock-firefox sponsorblock
