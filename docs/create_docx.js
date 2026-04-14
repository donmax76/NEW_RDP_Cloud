const fs = require("fs");
const { Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell,
        HeadingLevel, AlignmentType, BorderStyle, WidthType, ShadingType } = require("docx");

const border = { style: BorderStyle.SINGLE, size: 1, color: "999999" };
const borders = { top: border, bottom: border, left: border, right: border };
const headerShading = { fill: "2E4057", type: ShadingType.CLEAR };
const altShading = { fill: "F0F4F8", type: ShadingType.CLEAR };
const margins = { top: 60, bottom: 60, left: 100, right: 100 };

function hCell(text, w) {
  return new TableCell({ borders, width: { size: w, type: WidthType.DXA }, shading: headerShading, margins,
    children: [new Paragraph({ children: [new TextRun({ text, bold: true, color: "FFFFFF", font: "Arial", size: 20 })] })] });
}
function dCell(text, w, alt, bold) {
  return new TableCell({ borders, width: { size: w, type: WidthType.DXA }, shading: alt ? altShading : undefined, margins,
    children: [new Paragraph({ children: [new TextRun({ text, font: "Arial", size: 19, bold: !!bold })] })] });
}

function makeTable(rows) {
  const cw = [600, 2600, 4560, 1600];
  return new Table({ width: { size: 9360, type: WidthType.DXA }, columnWidths: cw,
    rows: [
      new TableRow({ children: [hCell("#", cw[0]), hCell("Filename", cw[1]), hCell("What to capture", cw[2]), hCell("Size", cw[3])] }),
      ...rows.map((r, i) => new TableRow({ children: [
        dCell(r[0], cw[0], i%2===1, true), dCell(r[1], cw[1], i%2===1, true),
        dCell(r[2], cw[2], i%2===1), dCell(r[3], cw[3], i%2===1)
      ]}))
    ]
  });
}

const doc = new Document({
  styles: {
    default: { document: { run: { font: "Arial", size: 22 } } },
    paragraphStyles: [
      { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 36, bold: true, font: "Arial", color: "2E4057" },
        paragraph: { spacing: { before: 360, after: 200 } } },
      { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { size: 28, bold: true, font: "Arial", color: "2E4057" },
        paragraph: { spacing: { before: 300, after: 160 } } },
    ]
  },
  sections: [{ properties: { page: { size: { width: 12240, height: 15840 }, margin: { top: 1200, right: 1440, bottom: 1200, left: 1440 } } },
    children: [
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 100 },
        children: [new TextRun({ text: "Prometey", size: 52, bold: true, font: "Arial", color: "2E4057" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 80 },
        children: [new TextRun({ text: "Screenshot List for PDF Guide", size: 28, font: "Arial", color: "666666" })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 300 },
        children: [new TextRun({ text: "20 screenshots required from the client interface", size: 22, font: "Arial", color: "999999" })] }),

      new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun("General Rules")] }),
      new Paragraph({ spacing: { after: 80 }, children: [new TextRun({ text: "Format: ", bold: true }), new TextRun("PNG (not JPEG)")] }),
      new Paragraph({ spacing: { after: 80 }, children: [new TextRun({ text: "Browser: ", bold: true }), new TextRun("Chrome / Edge, language EN, window width 1400+ px")] }),
      new Paragraph({ spacing: { after: 80 }, children: [new TextRun({ text: "Private data: ", bold: true }), new TextRun("Mask passwords and IP addresses with a gray rectangle")] }),
      new Paragraph({ spacing: { after: 80 }, children: [new TextRun({ text: "OS scale: ", bold: true }), new TextRun("Set to 100% during capture for crisp text")] }),
      new Paragraph({ spacing: { after: 200 }, children: [new TextRun({ text: "Save to: ", bold: true }), new TextRun({ text: "docs/img/", bold: true, color: "C62828" }), new TextRun(" with exact filenames from the tables below")] }),

      new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun("Connection (3 screenshots)")] }),
      makeTable([
        ["1", "01_login.png", "Login page: Token + Password fields empty, Connect button visible", "900\u00D7600"],
        ["2", "02_ssl_warning.png", "Chrome \"Your connection is not private\" page (open in incognito)", "1200\u00D7700"],
        ["3", "03_connected.png", "Topbar after connecting: green dot, Host: ONLINE, version info on the right", "1600\u00D7100"],
      ]),

      new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun("Panels (12 screenshots)")] }),
      makeTable([
        ["4", "04_dashboard.png", "Full Dashboard: RAM/CPU/GPU/Uptime cards + Activity Log + Quick Actions", "1600\u00D71000"],
        ["5", "05_speed_test.png", "Speed Test block after Run All Tests \u2014 all 4 cards with colored values", "1600\u00D7500"],
        ["6", "06_screen.png", "Screen panel with active remote desktop video stream", "1600\u00D71000"],
        ["7", "07_files.png", "Files: disk/folder tree on left + file list with columns on right", "1600\u00D71000"],
        ["8", "08_processes.png", "Processes: table with PID / Name / CPU% / Memory / Threads columns", "1600\u00D7800"],
        ["9", "09_terminal.png", "Terminal panel after running ipconfig or systeminfo command", "1600\u00D7700"],
        ["10", "10_audio.png", "Audio Recording: recording list on left + waveform player on right", "1600\u00D7700"],
        ["11", "11_screenshots.png", "Screenshots: grid view with 4-6 thumbnail previews", "1600\u00D7800"],
        ["12", "12_eventlog.png", "Event Log with entries + Auto-clean toolbar (Once / Loop / Off)", "1600\u00D7800"],
        ["13", "13_services.png", "Services: list of Windows services with Name / Status / Start Type", "1600\u00D7800"],
        ["14", "14_programs.png", "Installed Programs: table with Name / Version / Publisher / Size", "1600\u00D7800"],
        ["15", "15_registry.png", "Registry Editor: key tree on left + values on right", "1600\u00D7800"],
      ]),

      new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun("Settings (5 screenshots)")] }),
      makeTable([
        ["16", "16_settings_update.png", "Block Remote Host Update: DLL URL field + Update Host + Restart buttons", "1600\u00D7400"],
        ["17", "17_settings_deploy.png", "Block VPS Deploy: 3 file inputs + green Upload All 3 button", "1600\u00D7400"],
        ["18", "18_settings_config.png", "Host Config Editor: loaded JSON in textarea + New / Open / Save buttons", "1600\u00D7500"],
        ["19", "19_settings_threat.png", "Threat Monitor block: checkboxes + orange border + Check now button", "1600\u00D7300"],
        ["20", "20_lang_switch.png", "Language selector in topbar OPEN \u2014 dropdown showing EN / RU / AZ", "400\u00D7200"],
      ]),

      new Paragraph({ spacing: { before: 400 }, children: [] }),
      new Paragraph({ shading: { fill: "FFF3E0", type: ShadingType.CLEAR }, spacing: { after: 200 },
        children: [
          new TextRun({ text: "\u26A0 Note: ", bold: true, color: "E65100" }),
          new TextRun({ text: "VPS terminal and Windows host installation screenshots will be generated synthetically. Only the 20 client UI screenshots above need to be captured manually.", color: "E65100" }),
        ]
      }),
    ]
  }]
});

Packer.toBuffer(doc).then(buffer => {
  fs.writeFileSync("D:/Android_Projects/NEW_RDP_Cloud/docs/screenshots_list.docx", buffer);
  console.log("Created: docs/screenshots_list.docx (" + buffer.length + " bytes)");
});
