#ifndef L1TMuonEndCap_TrackFinder_hh
#define L1TMuonEndCap_TrackFinder_hh

#include <memory>
#include <string>
#include <vector>
#include <array>

#include "FWCore/Framework/interface/Event.h"
#include "FWCore/Framework/interface/EventSetup.h"
#include "FWCore/Framework/interface/ConsumesCollector.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"

#include "L1Trigger/L1TMuonEndCap/interface/SectorProcessor.hh"


class TrackFinder {
public:
  explicit TrackFinder(const edm::ParameterSet& iConfig, edm::ConsumesCollector&& iConsumes);
  ~TrackFinder();

  void process(
      // Input
      const edm::Event& iEvent, const edm::EventSetup& iSetup,
      // Output
      EMTFHitCollection& out_hits,
      EMTFTrackCollection& out_tracks
  );

private:
  GeometryTranslator geometry_translator_;

  ConditionHelper condition_helper_;

  SectorProcessorLUT sector_processor_lut_;

  std::unique_ptr<PtAssignmentEngine> pt_assign_engine_;

  sector_array<SectorProcessor> sector_processors_;

  const edm::ParameterSet config_;

  const edm::EDGetToken tokenCSC_, tokenRPC_, tokenGEM_;

  int verbose_;

  bool useCSC_, useRPC_, useGEM_;

  std::string era_;
};

#endif
