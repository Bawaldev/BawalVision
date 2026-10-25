// ============================================================================
// BawalVision Autonomous Client-Side PDF Compiler
// Pure TypeScript / Zero-Dependency PDF 1.4 Generator matching BawalVision C++ Master Standard
// ============================================================================

export interface ClientPdfOptions {
    title?: string;
    author?: string;
    subject?: string;
    company?: string;
}

interface ParsedTable {
    headers: string[];
    alignments: Array<'left' | 'center' | 'right'>;
    rows: string[][];
}

interface JpegMeta {
    width: number;
    height: number;
    data: Uint8Array;
}

/**
 * Extracts width and height from raw JPEG byte array by parsing SOF markers.
 */
function parseJpegDimensions(bytes: Uint8Array): { width: number; height: number } | null {
    let offset = 2; // skip SOI (0xFFD8)
    while (offset < bytes.length - 8) {
        if (bytes[offset] !== 0xFF) {
            offset++;
            continue;
        }
        const marker = bytes[offset + 1];
        // SOF0 (0xC0) or SOF2 (0xC2)
        if (marker === 0xC0 || marker === 0xC2) {
            const h = (bytes[offset + 5] << 8) | bytes[offset + 6];
            const w = (bytes[offset + 7] << 8) | bytes[offset + 8];
            return { width: w, height: h };
        }
        // Skip marker length
        const length = (bytes[offset + 2] << 8) | bytes[offset + 3];
        offset += 2 + length;
    }
    return null;
}

/**
 * Converts a data URL to raw JPEG bytes and dimension metadata.
 */
async function dataUrlToJpeg(dataUrl: string): Promise<JpegMeta | null> {
    try {
        if (!dataUrl.startsWith('data:')) return null;
        const commaIdx = dataUrl.indexOf(',');
        if (commaIdx === -1) return null;

        const mimeMatch = dataUrl.slice(0, commaIdx).match(/data:([^;]+)/);
        const mime = mimeMatch ? mimeMatch[1] : 'image/jpeg';
        const base64 = dataUrl.slice(commaIdx + 1);
        const binaryStr = atob(base64);
        let bytes = new Uint8Array(binaryStr.length);
        for (let i = 0; i < binaryStr.length; i++) {
            bytes[i] = binaryStr.charCodeAt(i);
        }

        // If not JPEG (e.g. PNG), convert to JPEG using an offscreen canvas
        if (!mime.includes('jpeg') && typeof document !== 'undefined') {
            const img = new Image();
            img.src = dataUrl;
            await new Promise((res, rej) => {
                img.onload = res;
                img.onerror = rej;
            });
            const canvas = document.createElement('canvas');
            canvas.width = img.naturalWidth || 640;
            canvas.height = img.naturalHeight || 480;
            const ctx = canvas.getContext('2d');
            if (!ctx) return null;
            ctx.fillStyle = '#ffffff';
            ctx.fillRect(0, 0, canvas.width, canvas.height);
            ctx.drawImage(img, 0, 0);
            const jpegUrl = canvas.toDataURL('image/jpeg', 0.85);
            return dataUrlToJpeg(jpegUrl);
        }

        const dims = parseJpegDimensions(bytes);
        if (!dims || dims.width === 0 || dims.height === 0) {
            return { width: 640, height: 480, data: bytes };
        }
        return { width: dims.width, height: dims.height, data: bytes };
    } catch {
        return null;
    }
}

/**
 * Escapes characters for PDF literal strings `(...)`.
 */
function escapePdfText(text: string): string {
    return text
        .replace(/\\/g, '\\\\')
        .replace(/\(/g, '\\(')
        .replace(/\)/g, '\\)')
        // Replace unicode symbols with readable clean ASCII text
        .replace(/✓/g, '[PASS]')
        .replace(/❌|✕/g, '[REJECT]')
        .replace(/⚠️/g, '[REVIEW]')
        .replace(/🍱|🍚|🍗|🥚|🧇|🧊|🍌|🍊|🍎|🥬|🌱/g, '')
        .replace(/[^\x20-\x7E\xA0-\xFF]/g, ' ')
        .trim();
}

/**
 * Compiles markdown content and embedded images into a standard PDF 1.4 binary.
 */
export async function compileClientMarkdownPdf(
    filename: string,
    markdown: string,
    title = 'BawalVision Inspection Audit Report'
): Promise<Blob> {
    const pageWidth = 595.28;  // A4 width in pt
    const pageHeight = 841.89; // A4 height in pt
    const marginLeft = 46.0;
    const marginRight = 46.0;
    const marginTop = 50.0;
    const marginBottom = 48.0;
    const contentWidth = pageWidth - marginLeft - marginRight;

    interface PageContent {
        commands: string[];
    }

    const pages: PageContent[] = [];
    let currentCmds: string[] = [];
    let cursorY = pageHeight - marginTop;

    function startNewPage() {
        if (currentCmds.length > 0) {
            pages.push({ commands: currentCmds });
        }
        currentCmds = [];
        cursorY = pageHeight - marginTop;
    }

    function ensureSpace(neededHeight: number) {
        if (cursorY - neededHeight < marginBottom) {
            startNewPage();
        }
    }

    // PDF Object Storage
    const images: Array<{ id: number; meta: JpegMeta }> = [];
    let objectCount = 6; // 1: Catalog, 2: Pages, 3: F1, 4: F2, 5: F3, 6: F4

    // Parse Markdown Lines
    const lines = markdown.split(/\r?\n/);
    let inTable = false;
    let tableLines: string[] = [];

    async function flushTable() {
        if (tableLines.length === 0) return;
        const parsed = parseMarkdownTable(tableLines);
        tableLines = [];
        inTable = false;
        if (!parsed) return;

        const rowHeight = 18.0;
        const colCount = parsed.headers.length;
        if (colCount === 0) return;

        const colWidth = contentWidth / colCount;
        const totalHeight = (parsed.rows.length + 1) * rowHeight + 10;
        ensureSpace(Math.min(totalHeight, rowHeight * 4));

        // Draw Table Header
        currentCmds.push(
            `0.08 0.18 0.36 rg`, // Navy/Blue fill
            `${marginLeft} ${cursorY - rowHeight + 4} ${contentWidth} ${rowHeight} re f`,
            `1 1 1 rg`,          // White text
            `BT /F2 8.5 Tf`
        );

        for (let c = 0; c < colCount; c++) {
            const hText = escapePdfText(parsed.headers[c]);
            const xPos = marginLeft + c * colWidth + 5;
            currentCmds.push(`${xPos} ${cursorY - rowHeight + 8} Td (${hText}) Tj -${xPos} -${cursorY - rowHeight + 8} Td`);
        }
        currentCmds.push(`ET`);
        cursorY -= rowHeight;

        // Draw Table Rows
        for (let r = 0; r < parsed.rows.length; r++) {
            ensureSpace(rowHeight + 4);
            const row = parsed.rows[r];
            const isAlt = r % 2 === 1;

            if (isAlt) {
                currentCmds.push(
                    `0.96 0.97 0.99 rg`, // Light alternating background
                    `${marginLeft} ${cursorY - rowHeight + 4} ${contentWidth} ${rowHeight} re f`
                );
            }

            // Cell border outline
            currentCmds.push(
                `0.88 0.90 0.94 RG 0.5 w`,
                `${marginLeft} ${cursorY - rowHeight + 4} ${contentWidth} ${rowHeight} re S`
            );

            currentCmds.push(`BT /F1 8.0 Tf`);
            for (let c = 0; c < colCount; c++) {
                const cellText = row[c] ?? '';
                const clean = escapePdfText(cellText.replace(/\*\*/g, '').replace(/`/g, ''));
                const xPos = marginLeft + c * colWidth + 5;

                // Color code verdicts
                if (clean.includes('PASS') || clean.includes('ACCEPTED')) {
                    currentCmds.push(`0.06 0.60 0.28 rg`);
                } else if (clean.includes('REVIEW')) {
                    currentCmds.push(`0.85 0.45 0.05 rg`);
                } else if (clean.includes('REJECT') || clean.includes('CRITICAL')) {
                    currentCmds.push(`0.85 0.15 0.15 rg`);
                } else {
                    currentCmds.push(`0.15 0.20 0.28 rg`);
                }

                currentCmds.push(`${xPos} ${cursorY - rowHeight + 8} Td (${clean}) Tj -${xPos} -${cursorY - rowHeight + 8} Td`);
            }
            currentCmds.push(`ET`);
            cursorY -= rowHeight;
        }
        cursorY -= 8;
    }

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i].trim();

        // Check for table line
        if (line.startsWith('|') && line.endsWith('|')) {
            inTable = true;
            tableLines.push(line);
            continue;
        } else if (inTable) {
            await flushTable();
        }

        if (line === '\\newpage' || line === '---') {
            startNewPage();
            continue;
        }

        if (!line) {
            cursorY -= 6;
            continue;
        }

        // Check for Markdown Image `![Alt](data:image/...)`
        const imgMatch = line.match(/!\[(.*?)\]\((data:image\/[^)]+)\)/);
        if (imgMatch) {
            const dataUrl = imgMatch[2];
            const jpegMeta = await dataUrlToJpeg(dataUrl);
            if (jpegMeta) {
                objectCount++;
                const imgObjId = objectCount;
                images.push({ id: imgObjId, meta: jpegMeta });

                // Scale image into content width while preserving aspect ratio
                let drawW = Math.min(contentWidth * 0.72, 380);
                let drawH = (drawW * jpegMeta.height) / jpegMeta.width;
                if (drawH > 220) {
                    drawH = 220;
                    drawW = (drawH * jpegMeta.width) / jpegMeta.height;
                }

                ensureSpace(drawH + 20);
                const drawX = marginLeft + (contentWidth - drawW) / 2;

                // Background container frame with drop border
                currentCmds.push(
                    `0.94 0.96 0.98 rg ${drawX - 4} ${cursorY - drawH - 4} ${drawW + 8} ${drawH + 8} re f`,
                    `0.80 0.85 0.92 RG 1 w ${drawX - 4} ${cursorY - drawH - 4} ${drawW + 8} ${drawH + 8} re S`,
                    `q ${drawW} 0 0 ${drawH} ${drawX} ${cursorY - drawH} cm /Im${imgObjId} Do Q`
                );
                cursorY -= (drawH + 16);
            }
            continue;
        }

        // H1 Heading
        if (line.startsWith('# ')) {
            ensureSpace(36);
            const heading = escapePdfText(line.replace(/^#\s+/, ''));
            // Top Accent Bar
            currentCmds.push(
                `0.08 0.18 0.36 rg`,
                `${marginLeft} ${cursorY - 4} ${contentWidth} 2.5 re f`,
                `BT /F2 15 Tf 0.06 0.10 0.20 rg`,
                `${marginLeft} ${cursorY - 20} Td (${heading}) Tj`,
                `ET`
            );
            cursorY -= 32;
            continue;
        }

        // H2 Heading
        if (line.startsWith('## ')) {
            ensureSpace(28);
            const heading = escapePdfText(line.replace(/^##\s+/, ''));
            currentCmds.push(
                `BT /F2 12 Tf 0.12 0.24 0.44 rg`,
                `${marginLeft} ${cursorY - 14} Td (${heading}) Tj`,
                `ET`,
                `0.85 0.88 0.94 RG 0.75 w`,
                `${marginLeft} ${cursorY - 18} m ${marginLeft + contentWidth} ${cursorY - 18} l S`
            );
            cursorY -= 26;
            continue;
        }

        // H3 Heading
        if (line.startsWith('### ')) {
            ensureSpace(22);
            const heading = escapePdfText(line.replace(/^###\s+/, ''));
            currentCmds.push(
                `BT /F2 10.5 Tf 0.18 0.25 0.35 rg`,
                `${marginLeft} ${cursorY - 12} Td (${heading}) Tj`,
                `ET`
            );
            cursorY -= 18;
            continue;
        }

        // Standard Body Paragraph / Key-Value
        ensureSpace(14);
        const cleanLine = escapePdfText(line.replace(/\*\*/g, '').replace(/`/g, ''));
        const isMuted = line.startsWith('_') || line.startsWith('*');
        currentCmds.push(
            `BT ${isMuted ? '/F1 8.0 Tf 0.45 0.50 0.58 rg' : '/F1 9.0 Tf 0.15 0.20 0.28 rg'}`,
            `${marginLeft} ${cursorY - 10} Td (${cleanLine}) Tj`,
            `ET`
        );
        cursorY -= 14;
    }

    if (inTable) {
        await flushTable();
    }

    if (currentCmds.length > 0) {
        pages.push({ commands: currentCmds });
    }

    if (pages.length === 0) {
        pages.push({ commands: [`BT /F1 10 Tf 50 750 Td (BawalVision Audit Report) Tj ET`] });
    }

    // Add running footer to every page
    const totalPages = pages.length;
    for (let pIdx = 0; pIdx < totalPages; pIdx++) {
        const p = pages[pIdx];
        const pageNum = `Page ${pIdx + 1} of ${totalPages}  |  BawalVision Industrial AI Quality Compliance`;
        p.commands.push(
            `0.88 0.90 0.94 RG 0.5 w ${marginLeft} ${marginBottom + 12} m ${marginLeft + contentWidth} ${marginBottom + 12} l S`,
            `BT /F1 7.5 Tf 0.50 0.55 0.62 rg ${marginLeft} ${marginBottom + 2} Td (${pageNum}) Tj ET`
        );
    }

    // Assemble Standard PDF 1.4 Byte Structure
    const chunks: Uint8Array[] = [];
    const offsets: number[] = [0]; // offset for obj 0

    function writeString(str: string) {
        const enc = new TextEncoder();
        const b = enc.encode(str);
        chunks.push(b);
        return b.length;
    }

    function writeBytes(bytes: Uint8Array) {
        chunks.push(bytes);
        return bytes.length;
    }

    let byteLength = 0;
    byteLength += writeString('%PDF-1.4\n%\xE2\xE3\xCF\xD3\n');

    // Obj 1: Catalog
    offsets[1] = byteLength;
    byteLength += writeString(`1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n`);

    // We will reserve obj IDs:
    // 2: Pages
    // 3: Font Helvetica (/F1)
    // 4: Font Helvetica-Bold (/F2)
    // 5: Font Courier (/F3)
    // 6..6+pages.length-1: Page objects
    // 6+pages.length..6+2*pages.length-1: Page content stream objects
    // subsequent: Image XObjects

    const firstPageObjId = 6;
    const firstContentObjId = firstPageObjId + totalPages;
    const firstImgObjId = firstContentObjId + totalPages;

    // Build XObject dictionary for resources
    let xobjDict = '';
    if (images.length > 0) {
        xobjDict = ' /XObject << ';
        for (let imgIdx = 0; imgIdx < images.length; imgIdx++) {
            xobjDict += `/Im${firstImgObjId + imgIdx} ${firstImgObjId + imgIdx} 0 R `;
        }
        xobjDict += '>> ';
    }

    // Obj 2: Pages Tree
    offsets[2] = byteLength;
    const kids = Array.from({ length: totalPages }, (_, i) => `${firstPageObjId + i} 0 R`).join(' ');
    byteLength += writeString(`2 0 obj\n<< /Type /Pages /Kids [ ${kids} ] /Count ${totalPages} >>\nendobj\n`);

    // Obj 3: Font F1
    offsets[3] = byteLength;
    byteLength += writeString(`3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\nendobj\n`);

    // Obj 4: Font F2
    offsets[4] = byteLength;
    byteLength += writeString(`4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>\nendobj\n`);

    // Obj 5: Font F3
    offsets[5] = byteLength;
    byteLength += writeString(`5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>\nendobj\n`);

    // Page Objects (Obj 6..6+totalPages-1)
    for (let pIdx = 0; pIdx < totalPages; pIdx++) {
        const objId = firstPageObjId + pIdx;
        const contentId = firstContentObjId + pIdx;
        offsets[objId] = byteLength;
        byteLength += writeString(
            `${objId} 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${pageWidth} ${pageHeight}] /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >>${xobjDict}>> /Contents ${contentId} 0 R >>\nendobj\n`
        );
    }

    // Page Content Stream Objects (Obj firstContentObjId..firstContentObjId+totalPages-1)
    for (let pIdx = 0; pIdx < totalPages; pIdx++) {
        const contentId = firstContentObjId + pIdx;
        offsets[contentId] = byteLength;
        const streamData = pages[pIdx].commands.join('\n');
        const streamBytes = new TextEncoder().encode(streamData);
        byteLength += writeString(
            `${contentId} 0 obj\n<< /Length ${streamBytes.length} >>\nstream\n`
        );
        byteLength += writeBytes(streamBytes);
        byteLength += writeString(`\nendstream\nendobj\n`);
    }

    // Image XObjects (Obj firstImgObjId..firstImgObjId+images.length-1)
    for (let imgIdx = 0; imgIdx < images.length; imgIdx++) {
        const imgObjId = firstImgObjId + imgIdx;
        const img = images[imgIdx];
        offsets[imgObjId] = byteLength;
        byteLength += writeString(
            `${imgObjId} 0 obj\n<< /Type /XObject /Subtype /Image /Width ${img.meta.width} /Height ${img.meta.height} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length ${img.meta.data.length} >>\nstream\n`
        );
        byteLength += writeBytes(img.meta.data);
        byteLength += writeString(`\nendstream\nendobj\n`);
    }

    // xref table
    const startXref = byteLength;
    const totalObjects = firstImgObjId + images.length;
    byteLength += writeString(`xref\n0 ${totalObjects}\n0000000000 65535 f \n`);
    for (let objIdx = 1; objIdx < totalObjects; objIdx++) {
        const off = String(offsets[objIdx] || 0).padStart(10, '0');
        byteLength += writeString(`${off} 00000 n \n`);
    }

    // trailer
    byteLength += writeString(
        `trailer\n<< /Size ${totalObjects} /Root 1 0 R /Info << /Title (${escapePdfText(title)}) /Creator (BawalVision Multi-Core Industrial AI) >> >>\nstartxref\n${startXref}\n%%EOF\n`
    );

    return new Blob(chunks as unknown as BlobPart[], { type: 'application/pdf' });
}

function parseMarkdownTable(lines: string[]): ParsedTable | null {
    if (lines.length < 2) return null;
    const cleanCells = (row: string) =>
        row
            .split('|')
            .slice(1, -1)
            .map((s) => s.trim());

    const headers = cleanCells(lines[0]);
    if (headers.length === 0) return null;

    // Separator line e.g. | :--- | :---: | ---: |
    const sepCells = cleanCells(lines[1]);
    const alignments: Array<'left' | 'center' | 'right'> = [];
    for (let c = 0; c < headers.length; c++) {
        const sep = sepCells[c] || '';
        if (sep.startsWith(':') && sep.endsWith(':')) alignments.push('center');
        else if (sep.endsWith(':')) alignments.push('right');
        else alignments.push('left');
    }

    const rows: string[][] = [];
    for (let r = 2; r < lines.length; r++) {
        const rawRow = lines[r].trim();
        if (!rawRow.startsWith('|')) continue;
        rows.push(cleanCells(rawRow));
    }

    return { headers, alignments, rows };
}
