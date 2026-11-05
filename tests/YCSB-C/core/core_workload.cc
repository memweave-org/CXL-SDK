//
//  core_workload.cc
//  YCSB-C
//
//  Created by Jinglei Ren on 12/9/14.
//  Copyright (c) 2014 Jinglei Ren <jinglei@ren.systems>.
//

#include "core/core_workload.h"
#include "core/db_config.h"
#include "core/ub_session.h"

#include <cassert>
#include <string>

#include "core/const_generator.h"
#include "core/scrambled_zipfian_generator.h"
#include "core/skewed_latest_generator.h"
#include "core/uniform_generator.h"
#include "core/zipfian_generator.h"

using std::string;
using ycsbc::CoreWorkload;

namespace {
void initialize_value(void* value, size_t bytes) {
#ifdef YCSB_UB_TWO_HOST
  if(ycsbc_ub::nc_data()) {
    const auto* pattern=reinterpret_cast<const unsigned char*>(&bytes);
    if(ycsbc_ub::nc_direct_data()) {
      auto* dst=static_cast<volatile unsigned char*>(value);
      for(size_t i=0;i<bytes;++i) dst[i]=pattern[i%sizeof(bytes)];
    } else {
      std::vector<unsigned char> scratch(bytes);
      for(size_t i=0;i<bytes;++i) scratch[i]=pattern[i%sizeof(bytes)];
      void* (*volatile copy)(void*,const void*,size_t)=&std::memcpy;
      copy(value,scratch.data(),bytes);
    }
    mfence();
    return;
  }
#endif
  for(size_t i=0;i+sizeof(bytes)<=bytes;i+=sizeof(bytes))
    memcpy(static_cast<char*>(value)+i,&bytes,sizeof(bytes));
#ifdef YCSB_UB_TWO_HOST
  clwb(value,bytes);
#endif
}
}

const string CoreWorkload::TABLENAME_PROPERTY = "table";
const string CoreWorkload::TABLENAME_DEFAULT = "usertable";

const string CoreWorkload::FIELD_COUNT_PROPERTY = "fieldcount";
const string CoreWorkload::FIELD_COUNT_DEFAULT = "10";

const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_PROPERTY =
    "field_len_dist";
const string CoreWorkload::FIELD_LENGTH_DISTRIBUTION_DEFAULT = "constant";

const string CoreWorkload::FIELD_LENGTH_PROPERTY = "fieldlength";
const string CoreWorkload::FIELD_LENGTH_DEFAULT = "100";

const string CoreWorkload::READ_ALL_FIELDS_PROPERTY = "readallfields";
const string CoreWorkload::READ_ALL_FIELDS_DEFAULT = "true";

const string CoreWorkload::WRITE_ALL_FIELDS_PROPERTY = "writeallfields";
const string CoreWorkload::WRITE_ALL_FIELDS_DEFAULT = "false";

const string CoreWorkload::READ_PROPORTION_PROPERTY = "readproportion";
const string CoreWorkload::READ_PROPORTION_DEFAULT = "0.95";

const string CoreWorkload::UPDATE_PROPORTION_PROPERTY = "updateproportion";
const string CoreWorkload::UPDATE_PROPORTION_DEFAULT = "0.05";

const string CoreWorkload::INSERT_PROPORTION_PROPERTY = "insertproportion";
const string CoreWorkload::INSERT_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::SCAN_PROPORTION_PROPERTY = "scanproportion";
const string CoreWorkload::SCAN_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::READMODIFYWRITE_PROPORTION_PROPERTY =
    "readmodifywriteproportion";
const string CoreWorkload::READMODIFYWRITE_PROPORTION_DEFAULT = "0.0";

const string CoreWorkload::REQUEST_DISTRIBUTION_PROPERTY =
    "requestdistribution";
const string CoreWorkload::REQUEST_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::ZERO_PADDING_PROPERTY = "zeropadding";
const string CoreWorkload::ZERO_PADDING_DEFAULT = "1";

const string CoreWorkload::MAX_SCAN_LENGTH_PROPERTY = "maxscanlength";
const string CoreWorkload::MAX_SCAN_LENGTH_DEFAULT = "1000";

const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_PROPERTY =
    "scanlengthdistribution";
const string CoreWorkload::SCAN_LENGTH_DISTRIBUTION_DEFAULT = "uniform";

const string CoreWorkload::INSERT_ORDER_PROPERTY = "insertorder";
const string CoreWorkload::INSERT_ORDER_DEFAULT = "hashed";

const string CoreWorkload::INSERT_START_PROPERTY = "insertstart";
const string CoreWorkload::INSERT_START_DEFAULT = "0";

const string CoreWorkload::RECORD_COUNT_PROPERTY = "recordcount";
const string CoreWorkload::OPERATION_COUNT_PROPERTY = "operationcount";

void CoreWorkload::Init(utils::Properties &p) {
  table_name_ = p.GetProperty(TABLENAME_PROPERTY, TABLENAME_DEFAULT);

  field_count_ =
      std::stoi(p.GetProperty(FIELD_COUNT_PROPERTY, FIELD_COUNT_DEFAULT));
  field_len_generator_ = GetFieldLenGenerator(p);

  double read_proportion = std::stod(
      p.GetProperty(READ_PROPORTION_PROPERTY, READ_PROPORTION_DEFAULT));
  double update_proportion = std::stod(
      p.GetProperty(UPDATE_PROPORTION_PROPERTY, UPDATE_PROPORTION_DEFAULT));
  double insert_proportion = std::stod(
      p.GetProperty(INSERT_PROPORTION_PROPERTY, INSERT_PROPORTION_DEFAULT));
  double scan_proportion = std::stod(
      p.GetProperty(SCAN_PROPORTION_PROPERTY, SCAN_PROPORTION_DEFAULT));
  double readmodifywrite_proportion = std::stod(p.GetProperty(
      READMODIFYWRITE_PROPORTION_PROPERTY, READMODIFYWRITE_PROPORTION_DEFAULT));

  record_count_ = std::stoi(p.GetProperty(RECORD_COUNT_PROPERTY));
  std::string request_dist = p.GetProperty(REQUEST_DISTRIBUTION_PROPERTY,
                                           REQUEST_DISTRIBUTION_DEFAULT);
  zero_padding_ =
      std::stoi(p.GetProperty(ZERO_PADDING_PROPERTY, ZERO_PADDING_DEFAULT));
  int max_scan_len = std::stoi(
      p.GetProperty(MAX_SCAN_LENGTH_PROPERTY, MAX_SCAN_LENGTH_DEFAULT));
  std::string scan_len_dist = p.GetProperty(SCAN_LENGTH_DISTRIBUTION_PROPERTY,
                                            SCAN_LENGTH_DISTRIBUTION_DEFAULT);
  int insert_start =
      std::stoi(p.GetProperty(INSERT_START_PROPERTY, INSERT_START_DEFAULT));

  read_all_fields_ = utils::StrToBool(
      p.GetProperty(READ_ALL_FIELDS_PROPERTY, READ_ALL_FIELDS_DEFAULT));
  write_all_fields_ = utils::StrToBool(
      p.GetProperty(WRITE_ALL_FIELDS_PROPERTY, WRITE_ALL_FIELDS_DEFAULT));

  if (p.GetProperty(INSERT_ORDER_PROPERTY, INSERT_ORDER_DEFAULT) == "hashed") {
    ordered_inserts_ = false;
  } else {
    ordered_inserts_ = true;
  }

  key_generator_ = new CounterGenerator(insert_start);

  if (read_proportion > 0) {
    op_chooser_.AddValue(READ, read_proportion);
  }
  if (update_proportion > 0) {
    op_chooser_.AddValue(UPDATE, update_proportion);
  }
  if (insert_proportion > 0) {
    op_chooser_.AddValue(INSERT, insert_proportion);
  }
  if (scan_proportion > 0) {
    op_chooser_.AddValue(SCAN, scan_proportion);
  }
  if (readmodifywrite_proportion > 0) {
    op_chooser_.AddValue(READMODIFYWRITE, readmodifywrite_proportion);
  }

  insert_key_sequence_.Set(record_count_);

  if (request_dist == "uniform") {
    key_chooser_ = new UniformGenerator(0, record_count_ - 1);

  } else if (request_dist == "zipfian") {
    // If the number of keys changes, we don't want to change popular keys.
    // So we construct the scrambled zipfian generator with a keyspace
    // that is larger than what exists at the beginning of the test.
    // If the generator picks a key that is not inserted yet, we just ignore it
    // and pick another key.
    int op_count = std::stoi(p.GetProperty(OPERATION_COUNT_PROPERTY));
    int new_keys = (int)(op_count * insert_proportion * 2);  // a fudge factor
    key_chooser_ = new ScrambledZipfianGenerator(record_count_ + new_keys);

  } else if (request_dist == "latest") {
    key_chooser_ = new SkewedLatestGenerator(insert_key_sequence_);

  } else {
    throw utils::Exception("Unknown request distribution: " + request_dist);
  }

  field_chooser_ = new UniformGenerator(0, field_count_ - 1);

  if (scan_len_dist == "uniform") {
    scan_len_chooser_ = new UniformGenerator(1, max_scan_len);
  } else if (scan_len_dist == "zipfian") {
    scan_len_chooser_ = new ZipfianGenerator(1, max_scan_len);
  } else {
    throw utils::Exception("Distribution not allowed for scan length: " +
                           scan_len_dist);
  }

  #ifdef INT_KEY_ADDR
  std::cerr << "Using INT_KEY_ADDR workload settings." << " VALUE SIZE = " << VALUE_ADDR_SIZE << std::endl;
  prefill_val_size_generator_ = new ConstGenerator(VALUE_ADDR_SIZE);
  transaction_val_size_generator_ = new ConstGenerator(VALUE_ADDR_SIZE);
  #endif

  group_chooser_ = new CounterGenerator(0);
}

ycsbc::Generator<uint64_t> *CoreWorkload::GetFieldLenGenerator(
    const utils::Properties &p) {
  string field_len_dist = p.GetProperty(FIELD_LENGTH_DISTRIBUTION_PROPERTY,
                                        FIELD_LENGTH_DISTRIBUTION_DEFAULT);
  int field_len =
      std::stoi(p.GetProperty(FIELD_LENGTH_PROPERTY, FIELD_LENGTH_DEFAULT));
  if (field_len_dist == "constant") {
    return new ConstGenerator(field_len);
  } else if (field_len_dist == "uniform") {
    return new UniformGenerator(1, field_len);
  } else if (field_len_dist == "zipfian") {
    return new ZipfianGenerator(1, field_len);
  } else {
    throw utils::Exception("Unknown field length distribution: " +
                           field_len_dist);
  }
}

void CoreWorkload::BuildValues(std::vector<ycsbc::DB::KVPair> &values) {
  for (int i = 0; i < field_count_; ++i) {
    ycsbc::DB::KVPair pair;
#ifndef YCSB_KEY
    pair.first = utils::Random64();
    pair.second = pair.first;
#else
    pair.first.append("field").append(std::to_string(i));
    pair.second.append(field_len_generator_->Next(), utils::RandomPrintChar());
#endif
    values.push_back(pair);
  }
}

uint64_t CoreWorkload::BuildNumValue() { return utils::Random64(); }

void CoreWorkload::BuildUpdate(std::vector<ycsbc::DB::KVPair> &update) {
  ycsbc::DB::KVPair pair;
#ifndef YCSB_KEY
  pair.first = utils::Random64();
  pair.second = pair.first;
#else
  pair.first.append(NextFieldName());
  pair.second.append(field_len_generator_->Next(), utils::RandomPrintChar());
#endif
  update.push_back(pair);
}

void *CoreWorkload::BuildPrefillValue() {
  size_t value_size = prefill_val_size_generator_->Next();
  if(value_size == 0) {
    value_size = 8;
  }
  assert(value_size != 0);
#ifdef YCSB_UB_TWO_HOST
  void *value = cacheable.malloc(value_size);
#else
  void *value = malloc(value_size);
#endif
  assert(value != nullptr);
  initialize_value(value,value_size);
  return value;
}

void *CoreWorkload::BuildTransactionValue() {
  size_t value_size = transaction_val_size_generator_->Next();
  if (value_size == 0) {
    value_size = 8;
  }
#ifdef YCSB_UB_TWO_HOST
  void *value = cacheable.malloc(value_size);
#else
  void *value = malloc(value_size);
#endif
  assert(value != nullptr);
  initialize_value(value,value_size);
  return value;
}
