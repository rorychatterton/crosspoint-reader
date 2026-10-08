#pragma once
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

class Epub;

// KOSync document id for path under the given matching method.
std::string koreaderDocumentHash(const std::string& path, DocumentMatchMethod method);

// Upload payload for the local position. With Send Metadata on, epub supplies
// the title and author; a null epub sends the filename only.
KOReaderProgress buildKOReaderProgressPayload(const std::string& documentHash, const SavedProgressPosition& local,
                                              const CrossPointPosition& position, const std::string& epubPath,
                                              Epub* epub);
