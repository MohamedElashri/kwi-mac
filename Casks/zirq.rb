# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0

cask "zirq" do
  version "0.2.0"
  sha256 "825b700cb4447d726b13a3a37f7bf7bb5b0d14baf284d89b9161ef5c330cd958"

  url "https://github.com/MohamedElashri/zirq/releases/download/v#{version}/Kwi-arm64.zip"
  name "Zirq"
  desc "Terminal with bidirectional Arabic, Persian and Hebrew support"
  homepage "https://github.com/MohamedElashri/zirq"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on arch: :arm64
  depends_on macos: :sonoma

  app "Kwi.app", target: "Zirq.app"

  caveats <<~EOS
    Requires macOS 14.4 or later. Quit Zirq or Kwi before upgrading.
    This app is ad-hoc signed and is not notarized.
  EOS
end
