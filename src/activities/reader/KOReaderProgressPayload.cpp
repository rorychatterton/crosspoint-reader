#include "KOReaderProgressPayload.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include "KOReaderDocumentId.h"

std::string koreaderDocumentHash(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

KOReaderProgress buildKOReaderProgressPayload(const std::string& documentHash, const SavedProgressPosition& local,
                                              const CrossPointPosition& position, const std::string& epubPath,
                                              Epub* epub) {
  KOReaderProgress progress{};
  progress.document = documentHash;
  progress.progress = local.xpath;
  progress.percentage = local.percentage;

  // Rich position for server profiles that explicitly support the CrossPoint extension.
  // The HTTP client enforces the same boundary before serializing the extension.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = local.percentage < 0.0f ? 0.0f : local.percentage > 1.0f ? 1.0f : local.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(position.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(position.pageNumber);
    pos.totalPages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
    if (position.hasParagraphIndex) {
      pos.paragraphIndex = position.paragraphIndex;
    }
    pos.xpath = local.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (epub->loadSyncMetadata(syncMetadata)) {
          meta.isbn = std::move(syncMetadata.isbn);
          meta.asin = std::move(syncMetadata.asin);
          meta.series = std::move(syncMetadata.series);
          meta.seriesIndex = syncMetadata.seriesIndex;
        } else {
          LOG_DBG("KOSync", "Could not read extended EPUB metadata; sending core metadata only");
        }
      }
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    // Plugin sidecar fields ("<book>.meta.json", written at download time via
    // the catalog sidecar mechanism or /api/plugin-fs) ride along so a custom
    // sync server can route progress by a service book id.
    // A missing sidecar (the common case) leaves extraJson empty.
    Storage.readFileToString("KOSync", epubPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }

  return progress;
}
