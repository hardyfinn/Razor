const { chromium } = require("playwright");
(async () => {
  const b = await chromium.launch(); const p = await b.newPage();
  await p.goto("file://./guide.html"); await p.waitForTimeout(300);
  await p.pdf({ path: "./Razor-Guide.pdf", format: "A4", printBackground: true,
    displayHeaderFooter: true, headerTemplate: "<span></span>",
    footerTemplate: '<div style="font-family:Inter;font-size:8px;color:#888;width:100%;text-align:center">Razor guide · <span class="pageNumber"></span> / <span class="totalPages"></span></div>',
    margin: { top: "16mm", bottom: "18mm", left: "16mm", right: "16mm" } });
  await b.close();
})();
