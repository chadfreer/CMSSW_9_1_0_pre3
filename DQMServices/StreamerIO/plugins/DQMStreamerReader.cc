#include "IOPool/Streamer/interface/MsgTools.h"
#include "IOPool/Streamer/interface/StreamerInputFile.h"
#include "IOPool/Streamer/interface/DumpTools.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/Utilities/interface/Exception.h"
#include "FWCore/Utilities/interface/EDMException.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/ParameterSet/interface/ConfigurationDescriptions.h"
#include "FWCore/ParameterSet/interface/ParameterSetDescription.h"
#include "FWCore/ParameterSet/interface/Registry.h"
#include "FWCore/Sources/interface/EventSkipperByID.h"

#include "DataFormats/Provenance/interface/ProductRegistry.h"
#include "DataFormats/Provenance/interface/ProcessHistoryRegistry.h"

#include "DQMStreamerReader.h"

#include <fstream>
#include <queue>
#include <cstdlib>
#include <boost/regex.hpp>
#include <boost/format.hpp>
#include <boost/range.hpp>
#include <boost/filesystem.hpp>

#include <IOPool/Streamer/interface/DumpTools.h>

namespace edm {

DQMStreamerReader::DQMStreamerReader(ParameterSet const& pset,
                                     InputSourceDescription const& desc)
    : StreamerInputSource(pset, desc),
      fiterator_(pset, DQMFileIterator::JS_DATA),
      streamReader_(),
      eventSkipperByID_(EventSkipperByID::create(pset).release()) {

  runNumber_ = pset.getUntrackedParameter<unsigned int>("runNumber");
  runInputDir_ = pset.getUntrackedParameter<std::string>("runInputDir");
  hltSel_ =
      pset.getUntrackedParameter<std::vector<std::string> >("SelectEvents");

  minEventsPerLs_ = pset.getUntrackedParameter<int>("minEventsPerLumi");
  flagSkipFirstLumis_ = pset.getUntrackedParameter<bool>("skipFirstLumis");
  flagEndOfRunKills_ = pset.getUntrackedParameter<bool>("endOfRunKills");
  flagDeleteDatFiles_ = pset.getUntrackedParameter<bool>("deleteDatFiles");

  reset_();
}

DQMStreamerReader::~DQMStreamerReader() { closeFile_(); }

void DQMStreamerReader::reset_() {
  // We have to load at least a single header,
  // so the ProductRegistry gets initialized.
  //
  // This must happen here (inside the constructor),
  // as ProductRegistry gets frozen after we initialize:
  // https://cmssdt.cern.ch/SDT/lxr/source/FWCore/Framework/src/Schedule.cc#441

  for (;;) {
    bool next = prepareNextFile();
    // check for end of run
    if (!next) {
      edm::LogAbsolute("DQMStreamerReader")
          << "End of run reached before DQMStreamerReader was initialised.";
      return;
    }

    // check if we have a file openned
    if (streamReader_.get() != nullptr) {
      // we are now initialised
      break;
    }

    // wait
    fiterator_.delay();
  }

  // Fast-forward to the last open file.
  if (flagSkipFirstLumis_) {
    while (fiterator_.hasNext()) {
      openNextFile_();
    }
  }

  edm::LogAbsolute("DQMStreamerReader") << "DQMStreamerReader initialised.";
}

void DQMStreamerReader::openFile_(std::string newStreamerFile_) {
  processedEventPerLs_ = 0;
  edm::ParameterSet pset;

  streamReader_ = std::unique_ptr<StreamerInputFile>(
      new StreamerInputFile(newStreamerFile_, eventSkipperByID_));

  InitMsgView const* header = getHeaderMsg();
  deserializeAndMergeWithRegistry(*header, false);

  dumpInitHeader(header);

  Strings tnames;
  header->hltTriggerNames(tnames);

  pset.addParameter<Strings>("SelectEvents", hltSel_);
  eventSelector_.reset(new TriggerSelector(pset, tnames));

  // our initialization
  processedEventPerLs_ = 0;

  if (flagDeleteDatFiles_) {
    // unlink the file
    unlink(newStreamerFile_.c_str());
  }
}

void DQMStreamerReader::closeFile_() {
  if (streamReader_.get() != nullptr) {
    streamReader_->closeStreamerFile();
    streamReader_ = nullptr;
  }
}

bool DQMStreamerReader::openNextFile_() {
  closeFile_();

  const DQMFileIterator::LumiEntry& lumi = fiterator_.front();
  std::string p = fiterator_.make_path_data(lumi);
  fiterator_.pop();

  if (boost::filesystem::exists(p)) {
    openFile_(fiterator_.make_path_data(lumi));
    return true;
  } else {
    /* dat file missing */
    edm::LogAbsolute("DQMStreamerReader")
        << "Data file (specified in json) is missing: " << p << ", skipping.";

    return false;
  }
}

InitMsgView const* DQMStreamerReader::getHeaderMsg() {
  InitMsgView const* header = streamReader_->startMessage();

  if (header->code() != Header::INIT) {  // INIT Msg
    throw Exception(errors::FileReadError, "DQMStreamerReader::readHeader")
        << "received wrong message type: expected INIT, got " << header->code()
        << "\n";
  }

  return header;
}

EventMsgView const* DQMStreamerReader::getEventMsg() {
  if (!streamReader_->next()) {
    return nullptr;
  }

  EventMsgView const* msg = streamReader_->currentRecord();

  //  if (msg != nullptr) dumpEventView(msg);
  return msg;
}

/**
 * Prepare (open) the next file for reading.
 * It is used by prepareNextEvent and in the constructor.
 *
 * Does not block/wait.
 *
 * Return false if this is end of run and/or no more file are available.
 * However, return of "true" does not imply the file has been openned,
 * but we need to wait until some future file becomes available.
 */
bool DQMStreamerReader::prepareNextFile() {
  typedef DQMFileIterator::State State;

  for (;;) {
    // check for end of run file and force quit
    if (flagEndOfRunKills_ && (fiterator_.state() != State::OPEN)) {
      closeFile_();
      return false;
    }

    // check for end of run and quit if everything has been processed.
    // this clean exit
    if ((streamReader_.get() == nullptr) && (!fiterator_.hasNext()) &&
        (fiterator_.state() == State::EOR)) {

      closeFile_();
      return false;
    }

    // if this is end of run and no more files to process
    // close it
    if ((processedEventPerLs_ >= minEventsPerLs_) && (!fiterator_.hasNext()) &&
        (fiterator_.state() == State::EOR)) {

      closeFile_();
      return false;
    }

    // skip to the next file if we have no files openned yet
    if (streamReader_.get() == nullptr) {
      if (fiterator_.hasNext()) {
        openNextFile_();
        // we might need to open once more (if .dat is missing)
        continue;
      }
    }

    // or if there is a next file and enough eventshas been processed.
    if (fiterator_.hasNext() && (processedEventPerLs_ >= minEventsPerLs_)) {
      openNextFile_();
      // we might need to open once more (if .dat is missing)
      continue;
    }

    return true;
  }
}

/**
 * Waits and reads the event header.
 * If end-of-run nullptr is returned.
 */
EventMsgView const* DQMStreamerReader::prepareNextEvent() {
  fiterator_.updateWatchdog();

  EventMsgView const* eview = nullptr;
  typedef DQMFileIterator::State State;

  // wait for the next event
  for (;;) {
    // edm::LogAbsolute("DQMStreamerReader")
    //     << "State loop.";
    bool next = prepareNextFile();
    if (!next) return nullptr;

    // sleep
    if (streamReader_.get() == nullptr) {
      // the reader does not exist
      fiterator_.delay();
    } else {
      // our reader exists, try to read out an event
      eview = getEventMsg();

      if (eview == nullptr) {
        // read unsuccessful
        // this means end of file, so close the file
        closeFile_();
      } else {
        if (!acceptEvent(eview)) {
          continue;
        } else {
          return eview;
        }
      }
    }
  }
  return eview;
}

/**
 * This is the actual code for checking the new event and/or deserializing it.
 */
bool DQMStreamerReader::checkNextEvent() {
  EventMsgView const* eview = prepareNextEvent();
  if (eview == nullptr) {
    return false;
  }

  // this is reachable only if eview is set
  // and the file is openned
  if (streamReader_->newHeader()) {
    // A new file has been opened and we must compare Headers here !!
    // Get header/init from reader
    InitMsgView const* header = getHeaderMsg();
    deserializeAndMergeWithRegistry(*header, true);
  }

  processedEventPerLs_ += 1;
  deserializeEvent(*eview);

  return true;
}

bool DQMStreamerReader::acceptEvent(const EventMsgView* evtmsg) {

  std::vector<unsigned char> hltTriggerBits_;
  int hltTriggerCount_ = evtmsg->hltCount();
  if (hltTriggerCount_ > 0) {
    hltTriggerBits_.resize(1 + (hltTriggerCount_ - 1) / 4);
  }
  evtmsg->hltTriggerBits(&hltTriggerBits_[0]);

  if (eventSelector_->wantAll() ||
      eventSelector_->acceptEvent(&hltTriggerBits_[0], evtmsg->hltCount())) {
    std::cout << "----****----- Events is accepted " << std::endl;
    return true;
  } else {
    std::cout << "----****----- Events is not selected " << std::endl;
    return false;
  }
}

void DQMStreamerReader::skip(int toSkip) {
  for (int i = 0; i != toSkip; ++i) {
    EventMsgView const* evMsg = prepareNextEvent();

    if (evMsg == nullptr) {
      return;
    }

    // If the event would have been skipped anyway, don't count it as a skipped
    // event.
    if (eventSkipperByID_ && eventSkipperByID_->skipIt(
                                 evMsg->run(), evMsg->lumi(), evMsg->event())) {
      --i;
    }
  }
}

void DQMStreamerReader::fillDescriptions(
    ConfigurationDescriptions& descriptions) {
  ParameterSetDescription desc;
  desc.setComment("Reads events from streamer files.");

  desc.addUntracked<std::vector<std::string> >("SelectEvents")
      ->setComment("HLT path to select events ");

  desc.addUntracked<int>("minEventsPerLumi", 1)->setComment(
      "Minimum number of events to process per lumisection, "
      "before switching to a new input file. If the next file "
      "does not yet exist, "
      "the number of processed events will be bigger.");

  desc.addUntracked<bool>("skipFirstLumis", false)->setComment(
      "Skip (and ignore the minEventsPerLumi parameter) for the files "
      "which have been available at the begining of the processing. "
      "If set to true, the reader will open last available file for "
      "processing.");

  desc.addUntracked<bool>("deleteDatFiles", false)->setComment(
      "Delete data files after they have been closed, in order to "
      "save disk space.");

  desc.addUntracked<bool>("endOfRunKills", false)->setComment(
      "Kill the processing as soon as the end-of-run file appears, even if "
      "there are/will be unprocessed lumisections.");

  // desc.addUntracked<unsigned int>("skipEvents", 0U)
  //    ->setComment("Skip the first 'skipEvents' events that otherwise would "
  //                 "have been processed.");

  // This next parameter is read in the base class, but its default value
  // depends on the derived class, so it is set here.
  desc.addUntracked<bool>("inputFileTransitionsEachEvent", false);

  DQMFileIterator::fillDescription(desc);
  StreamerInputSource::fillDescription(desc);
  EventSkipperByID::fillDescription(desc);

  descriptions.add("source", desc);
}
}
