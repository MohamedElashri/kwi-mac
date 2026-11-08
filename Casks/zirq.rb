# SPDX-FileCopyrightText: 2026 Mohamed Elashri <kwi@elashri.com>
# SPDX-License-Identifier: CC0-1.0

cask "zirq" do
  version "0.3.0"
  sha256 "93c78fea71ca0e767969a93f31163288101ea3b72ddcf07f530f84d9160ea2fc"

  url "https://github.com/MohamedElashri/zirq/releases/download/v#{version}/Zirq-arm64.zip"
  name "Zirq"
  desc "Terminal with bidirectional Arabic, Persian and Hebrew support"
  homepage "https://github.com/MohamedElashri/zirq"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on arch: :arm64
  depends_on macos: :sonoma

  app "Zirq.app"

  caveats <<~EOS
    Requires macOS 14.4 or later. Quit Zirq or Kwi before upgrading.
    This app is ad-hoc signed and is not notarized.
  EOS
end
