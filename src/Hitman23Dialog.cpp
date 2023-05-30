//
// Created by Andrej Redeky.
// Copyright © 2015-2023 Feldarian Softworks. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2
//

#include "Precompiled.hpp"

#include "Hitman23Dialog.hpp"

HitmanSoundRecord Hitman23WHDRecord::ToHitmanSoundRecord() const
{
  return {
    0,
    dataSizeUncompressed,
    dataSize,
    sampleRate,
    formatTag,
    static_cast<uint16_t>(bitsPerSample),
    static_cast<uint16_t>(channels),
    static_cast<uint16_t>(blockAlign),
    static_cast<uint16_t>(fmtExtra)
  };
}

void Hitman23WHDRecord::FromHitmanSoundRecord(const HitmanSoundRecord &soundRecord)
{
  formatTag = soundRecord.formatTag;
  sampleRate = soundRecord.sampleRate;
  bitsPerSample = soundRecord.bitsPerSample;
  dataSizeUncompressed = soundRecord.dataSizeUncompressed;
  dataSize = soundRecord.dataSize;
  channels = soundRecord.channels;
  samplesCount = soundRecord.dataSizeUncompressed / sizeof int16_t;
  blockAlign = soundRecord.blockAlign;
  fmtExtra = soundRecord.fmtExtra;
}

bool Hitman23WAVFile::Clear(const bool retVal)
{
  header = nullptr;
  recordMap.clear();
  extraData.clear();
  path.clear();

  return retVal;
}

bool Hitman23WAVFile::Load(const StringView8CI loadPath, const std::map<StringView8CI, Hitman23WHDRecord *> &whdRecordsMap,
                           std::map<StringView8CI, HitmanFile>& fileMap, const bool isMissionWAV)
{
  const auto wavData = ReadWholeBinaryFile(loadPath);
  if (wavData.empty())
    return Clear(false);

  struct WAVFileData
  {
    uint32_t size = 0;
    HitmanFile* file = nullptr;
  };

  auto resampledOffset = static_cast<uint32_t>(wavData.size());
  size_t foundItems = 0;
  std::map<uint32_t, uint32_t> resampledMap;
  std::map<uint32_t, WAVFileData> offsetToWAVFileDataMap;
  for (auto& [whdRecordPath, whdRecord] : whdRecordsMap)
  {
    if ((whdRecord->dataInStreams == 0) != isMissionWAV)
      continue;

    auto offsetToWAVFileDataIt = offsetToWAVFileDataMap.find(whdRecord->dataOffset);
    if (offsetToWAVFileDataIt == offsetToWAVFileDataMap.end())
      offsetToWAVFileDataMap.insert({whdRecord->dataOffset, {whdRecord->dataSize, &fileMap[whdRecordPath]}});
    else
    {
      resampledMap[resampledOffset] = whdRecord->dataOffset;
      whdRecord->dataOffset = resampledOffset;
      resampledOffset += whdRecord->dataSize;

      offsetToWAVFileDataMap.insert({whdRecord->dataOffset, {whdRecord->dataSize, &fileMap[whdRecordPath]}});
    }

    ++foundItems;
  }

  if (offsetToWAVFileDataMap.empty() || offsetToWAVFileDataMap.size() != foundItems)
    return true;

  recordMap.clear();

  extraData.reserve(2 * offsetToWAVFileDataMap.size() + 1);

  uint32_t currOffset = 0;
  for (auto& [offset, wavFileData] : offsetToWAVFileDataMap)
  {
    if (currOffset < offset)
    {
      auto& newData = extraData.emplace_back();
      newData.resize(offset - currOffset, 0);
      newData.shrink_to_fit();

      std::memcpy(newData.data(), wavData.data() + currOffset, offset - currOffset);
      recordMap.try_emplace(currOffset, Hitman23WAVRecord{newData, currOffset});
    }

    auto& newData = wavFileData.file->data;
    newData.resize(wavFileData.size, 0);
    newData.shrink_to_fit();

    const auto trueOffset = offset >= wavData.size() ? resampledMap[offset] : offset;
    std::memcpy(newData.data(), wavData.data() + trueOffset, wavFileData.size);
    recordMap.try_emplace(offset, Hitman23WAVRecord{newData, offset});
    currOffset = offset + wavFileData.size;
  }

  if (currOffset < wavData.size())
  {
    auto& newData = extraData.emplace_back();
    newData.resize(wavData.size() - currOffset, 0);
    newData.shrink_to_fit();

    std::memcpy(newData.data(), wavData.data() + currOffset, wavData.size() - currOffset);
    recordMap.try_emplace(currOffset, Hitman23WAVRecord{newData, currOffset});
  }

  if (isMissionWAV)
    header = reinterpret_cast<Hitman23WAVHeader *>(recordMap[0].data.get().data());
  else
    header = nullptr;

  path = loadPath;

  return true;
}

bool Hitman23WAVFile::Save(const StringView8CI savePathView)
{
  const auto savePath = savePathView.path();
  create_directories(savePath.parent_path());

  const auto oldSync = std::ios_base::sync_with_stdio(false);

  std::ofstream wavData(savePath, std::ios::binary | std::ios::trunc);

  uint32_t offset = 0;
  for (auto &record : recordMap | std::views::values)
  {
    record.newOffset = offset;
    offset += static_cast<uint32_t>(record.data.get().size());
  }

  if (header != nullptr)
    header->fileSizeWithHeader = offset;

  for (auto &record : recordMap | std::views::values)
    wavData.write(record.data.get().data(), record.data.get().size());

  std::ios_base::sync_with_stdio(oldSync);

  path = savePath;

  return true;
}

bool Hitman23WHDFile::Clear(const bool retVal)
{
  header = nullptr;
  recordMap.clear();
  data.clear();
  path.clear();

  return retVal;
}

bool Hitman23WHDFile::Load(Hitman23Dialog& archiveDialog, const StringView8CI loadPathView)
{
  if (data.empty())
    data = ReadWholeBinaryFile(loadPathView);

  if (data.empty())
    return Clear(false);

  recordMap.clear();

  auto *whdPtr = data.data();
  header = reinterpret_cast<Hitman23WHDHeader *>(whdPtr);
  whdPtr += sizeof Hitman23WHDHeader;

  while (*whdPtr)
  {
    whdPtr += std::strlen(whdPtr) + 1; // + 0-3 bytes for H3, so it is aligned on 4 bytes...
    auto *whdRecord = reinterpret_cast<Hitman23WHDRecord *>(whdPtr);
    if (whdRecord->type != 0x06)
    {
      whdPtr += 4 - (reinterpret_cast<uintptr_t>(whdPtr) % 4);
      whdRecord = reinterpret_cast<Hitman23WHDRecord *>(whdPtr);
    }
    whdPtr += sizeof Hitman23WHDRecord;

    assert(whdRecord->type == 0x06);

    String8CI filePath(std::string_view(data.data() + whdRecord->filePathOffset));
    auto filePathNative = filePath.path();

    if (filePathNative.extension() != StringViewWCI(L".wav"))
      return Clear(false);

    if (whdRecord->dataInStreams == 0)
      filePath = ChangeExtension(String8CI(relative(loadPathView.path(), archiveDialog.basePath.path())), filePath);
    else
    {
      if (!filePathNative.has_parent_path())
        filePath = L"Streams" / filePathNative;
    }

    auto& file = archiveDialog.GetFile(filePath);

    if (!recordMap.try_emplace(file.path, whdRecord).second)
      return Clear(false);

    archiveDialog.whdRecordsMap[file.path].emplace_back(whdRecord);
    archiveDialog.fileMap[file.path].archiveRecord = whdRecord->ToHitmanSoundRecord();
  }

  path = loadPathView;

  return true;
}

bool Hitman23WHDFile::Save(const Hitman23WAVFile &streamsWAV, const Hitman23WAVFile &missionWAV, const StringView8CI savePath)
{
  for (auto *whdRecord : recordMap | std::views::values)
  {
    const auto &wavRecordMap = whdRecord->dataInStreams == 0 ? missionWAV.recordMap : streamsWAV.recordMap;
    const auto wavRecordIt = wavRecordMap.find(whdRecord->dataOffset);
    assert(wavRecordIt != wavRecordMap.end());
    if (wavRecordIt != wavRecordMap.end())
      whdRecord->dataOffset = wavRecordIt->second.newOffset;
  }

  const auto oldSync = std::ios_base::sync_with_stdio(false);

  std::ofstream whdData(savePath.path(), std::ios::binary | std::ios::trunc);
  whdData.write(data.data(), data.size());
  whdData.close();

  std::ios_base::sync_with_stdio(oldSync);

  path = savePath;

  return true;
}

bool Hitman23Dialog::Clear(const bool retVal)
{
  whdFiles.clear();
  wavFiles.clear();
  streamsWAV.Clear();
  basePath.clear();
  fileMap.clear();
  whdRecordsMap.clear();

  return HitmanDialog::Clear(retVal);
}

bool Hitman23Dialog::ImportSingle(const StringView8CI importFolderPathView, StringView8CI importFilePathView)
{
  auto filePath = String8CI(relative(importFilePathView.path(), importFolderPathView.path()));
  auto fileIt = fileMap.find(filePath);
  auto whdRecordsIt = whdRecordsMap.find(filePath);
  if (fileIt == fileMap.end() || whdRecordsIt == whdRecordsMap.end())
  {
    const StringView8CI nextExtension = filePath.path().extension() == StringView8CI(".wav") ? ".ogg" : ".wav";
    filePath = ChangeExtension(filePath, nextExtension);
    fileIt = fileMap.find(filePath);
    whdRecordsIt = whdRecordsMap.find(filePath);
    if (fileIt == fileMap.end() || whdRecordsIt == whdRecordsMap.end())
    {
      DisplayWarning(LocalizationManager::Get().LocalizeFormat("HITMAN_DIALOG_WARNING_MISSING_FILE", importFilePathView));
      return false;
    }
  }

  if (!ImportSingleHitmanFile(fileIt->second, filePath, importFilePathView))
    return false;

  for (auto* whdRecord : whdRecordsIt->second)
    whdRecord->FromHitmanSoundRecord(fileIt->second.archiveRecord);

  return true;
}

bool Hitman23Dialog::LoadImpl(const StringView8CI loadPathView)
{
  Clear();

  const auto rootPath = loadPathView.path().parent_path();
  const auto scenesPath = rootPath / L"Scenes";
  if (!exists(scenesPath))
  {
    DisplayError(LocalizationManager::Get().Localize("HITMAN_23_DIALOG_ERROR_MISSING_SCENES"));
    return false;
  }

  const auto allWHDFiles = GetAllFilesInDirectory(String8CI(scenesPath), ".whd", true);

  whdFiles.reserve(allWHDFiles.size());
  wavFiles.reserve(allWHDFiles.size());

  basePath = rootPath;

  std::map<StringView8CI, Hitman23WHDRecord *> allWHDRecords;
  for (const auto &whdPath : allWHDFiles)
  {
    auto &whdFile = whdFiles.emplace_back();
    if (!whdFile.Load(*this, whdPath))
      return Clear(false);

    for (const auto& whdRecordMapKV : whdFile.recordMap)
      allWHDRecords.insert(whdRecordMapKV);

    if (!wavFiles.emplace_back().Load(ChangeExtension(whdFile.path, ".wav"), whdFile.recordMap, fileMap, true))
      return Clear(false);
  }

  if (!streamsWAV.Load(loadPathView, allWHDRecords, fileMap, false))
    return Clear(false);

  if (!options.common.checkOriginality)
    return true;

  auto dataPath = GetProgramPath().path();
  if (dataPath.empty())
    return Clear(false);

  dataPath /= L"data";
  dataPath /= L"records";
  dataPath /= streamsWAV.recordMap[0].data.get().data()[0] == 0x6F ? L"h2" : L"h3";

  originalDataPath = dataPath;

  if (!LoadOriginalData())
    return Clear(false);

  return true;
}

bool Hitman23Dialog::SaveImpl(const StringView8CI savePathView)
{
  const auto newBasePath = savePathView.path().parent_path();

  streamsWAV.Save(savePathView);

  for (size_t i = 0; i < whdFiles.size(); ++i)
  {
    String8CI savePath(newBasePath / relative(wavFiles[i].path.path(), basePath.path()));
    wavFiles[i].Save(savePath);
    whdFiles[i].Save(streamsWAV, wavFiles[i], savePath);
  }

  basePath = newBasePath;

  CleanDirty();

  return true;
}

void Hitman23Dialog::DrawDialog()
{
  DrawHitmanDialog("Silent Assassin / Contracts", "Hitman 2/3 Streams (streams.wav)\0streams.wav\0", "streams.wav");
}
