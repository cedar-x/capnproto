// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "benchmark.pb.h"
#include "benchmark-common.h"
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <thread>
#include <snappy/snappy.h>
#include <snappy/snappy-sinksource.h>

namespace capnproto {
namespace benchmark {
namespace protobuf {

// =======================================================================================
// Test case:  Expression evaluation

int32_t makeExpression(Expression* exp, uint depth) {
  exp->set_op((Operation)(fastRand(Operation_MAX + 1)));

  int32_t left, right;

  if (fastRand(8) < depth) {
    left = fastRand(128) + 1;
    exp->set_left_value(left);
  } else {
    left = makeExpression(exp->mutable_left_expression(), depth + 1);
  }

  if (fastRand(8) < depth) {
    right = fastRand(128) + 1;
    exp->set_right_value(right);
  } else {
    right = makeExpression(exp->mutable_right_expression(), depth + 1);
  }

  switch (exp->op()) {
    case Operation::ADD:
      return left + right;
    case Operation::SUBTRACT:
      return left - right;
    case Operation::MULTIPLY:
      return left * right;
    case Operation::DIVIDE:
      return div(left, right);
    case Operation::MODULUS:
      return mod(left, right);
  }
  throw std::logic_error("Can't get here.");
}

int32_t evaluateExpression(const Expression& exp) {
  uint32_t left, right;

  if (exp.has_left_value()) {
    left = exp.left_value();
  } else {
    left = evaluateExpression(exp.left_expression());
  }

  if (exp.has_right_value()) {
    right = exp.right_value();
  } else {
    right = evaluateExpression(exp.right_expression());
  }

  switch (exp.op()) {
    case Operation::ADD:
      return left + right;
    case Operation::SUBTRACT:
      return left - right;
    case Operation::MULTIPLY:
      return left * right;
    case Operation::DIVIDE:
      return div(left, right);
    case Operation::MODULUS:
      return mod(left, right);
  }
  throw std::logic_error("Can't get here.");
}

class ExpressionTestCase {
public:
  typedef Expression Request;
  typedef EvaluationResult Response;
  typedef int32_t Expectation;

  static inline int32_t setupRequest(Expression* request) {
    return makeExpression(request, 0);
  }
  static inline void handleRequest(const Expression& request, EvaluationResult* response) {
    response->set_value(evaluateExpression(request));
  }
  static inline bool checkResponse(const EvaluationResult& response, int32_t expected) {
    return response.value() == expected;
  }
};

// =======================================================================================
// Test case:  Cat Rank
//
// The server receives a list of candidate search results with scores.  It promotes the ones that
// mention "cat" in their snippet and demotes the ones that mention "dog", sorts the results by
// descending score, and returns.
//
// The promotion multiplier is large enough that all the results mentioning "cat" but not "dog"
// should end up at the front ofthe list, which is how we verify the result.

struct ScoredResult {
  double score;
  const SearchResult* result;

  ScoredResult() = default;
  ScoredResult(double score, const SearchResult* result): score(score), result(result) {}

  inline bool operator<(const ScoredResult& other) const { return score > other.score; }
};

class CatRankTestCase {
public:
  typedef SearchResultList Request;
  typedef SearchResultList Response;
  typedef int Expectation;

  static int setupRequest(SearchResultList* request) {
    int count = fastRand(1000);
    int goodCount = 0;

    for (int i = 0; i < count; i++) {
      SearchResult* result = request->add_result();
      result->set_score(1000 - i);
      result->set_url("http://example.com/");
      std::string* url = result->mutable_url();
      int urlSize = fastRand(100);
      for (int j = 0; j < urlSize; j++) {
        url->push_back('a' + fastRand(26));
      }

      bool isCat = fastRand(8) == 0;
      bool isDog = fastRand(8) == 0;
      goodCount += isCat && !isDog;

      std::string* snippet = result->mutable_snippet();
      snippet->push_back(' ');

      int prefix = fastRand(20);
      for (int j = 0; j < prefix; j++) {
        snippet->append(WORDS[fastRand(WORDS_COUNT)]);
      }

      if (isCat) snippet->append("cat ");
      if (isDog) snippet->append("dog ");

      int suffix = fastRand(20);
      for (int j = 0; j < suffix; j++) {
        snippet->append(WORDS[fastRand(WORDS_COUNT)]);
      }
    }

    return goodCount;
  }

  static inline void handleRequest(const SearchResultList& request, SearchResultList* response) {
    std::vector<ScoredResult> scoredResults;

    for (auto& result: request.result()) {
      double score = result.score();
      if (result.snippet().find(" cat ") != std::string::npos) {
        score *= 10000;
      }
      if (result.snippet().find(" dog ") != std::string::npos) {
        score /= 10000;
      }
      scoredResults.emplace_back(score, &result);
    }

    std::sort(scoredResults.begin(), scoredResults.end());

    for (auto& result: scoredResults) {
      SearchResult* out = response->add_result();
      out->set_score(result.score);
      out->set_url(result.result->url());
      out->set_snippet(result.result->snippet());
    }
  }

  static inline bool checkResponse(const SearchResultList& response, int expectedGoodCount) {
    int goodCount = 0;
    for (auto& result: response.result()) {
      if (result.score() > 1001) {
        ++goodCount;
      } else {
        break;
      }
    }

    return goodCount == expectedGoodCount;
  }
};

// =======================================================================================
// Test case:  Car Sales
//
// We have a parking lot full of cars and we want to know how much they are worth.

uint64_t carValue(const Car& car) {
  // Do not think too hard about realism.

  uint64_t result = 0;

  result += car.seats() * 200;
  result += car.doors() * 350;
  for (auto& wheel: car.wheel()) {
    result += wheel.diameter() * wheel.diameter();
    result += wheel.snow_tires() ? 100 : 0;
  }

  result += car.length() * car.width() * car.height() / 50;

  const Engine& engine = car.engine();
  result += engine.horsepower() * 40;
  if (engine.uses_electric()) {
    if (engine.uses_gas()) {
      // hybrid
      result += 5000;
    } else {
      result += 3000;
    }
  }

  result += car.has_power_windows() ? 100 : 0;
  result += car.has_power_steering() ? 200 : 0;
  result += car.has_cruise_control() ? 400 : 0;
  result += car.has_nav_system() ? 2000 : 0;

  result += car.cup_holders() * 25;

  return result;
}

void randomCar(Car* car) {
  // Do not think too hard about realism.

  static const char* const MAKES[] = { "Toyota", "GM", "Ford", "Honda", "Tesla" };
  static const char* const MODELS[] = { "Camry", "Prius", "Volt", "Accord", "Leaf", "Model S" };

  car->set_make(MAKES[fastRand(sizeof(MAKES) / sizeof(MAKES[0]))]);
  car->set_model(MODELS[fastRand(sizeof(MODELS) / sizeof(MODELS[0]))]);

  car->set_color((Color)fastRand(Color_MAX));
  car->set_seats(2 + fastRand(6));
  car->set_doors(2 + fastRand(3));

  for (uint i = 0; i < 4; i++) {
    Wheel* wheel = car->add_wheel();
    wheel->set_diameter(25 + fastRand(15));
    wheel->set_air_pressure(30 + fastRandDouble(20));
    wheel->set_snow_tires(fastRand(16) == 0);
  }

  car->set_length(170 + fastRand(150));
  car->set_width(48 + fastRand(36));
  car->set_height(54 + fastRand(48));
  car->set_weight(car->length() * car->width() * car->height() / 200);

  Engine* engine = car->mutable_engine();
  engine->set_horsepower(100 * fastRand(400));
  engine->set_cylinders(4 + 2 * fastRand(3));
  engine->set_cc(800 + fastRand(10000));

  car->set_fuel_capacity(10.0 + fastRandDouble(30.0));
  car->set_fuel_level(fastRandDouble(car->fuel_capacity()));
  car->set_has_power_windows(fastRand(2));
  car->set_has_power_steering(fastRand(2));
  car->set_has_cruise_control(fastRand(2));
  car->set_cup_holders(fastRand(12));
  car->set_has_nav_system(fastRand(2));
}

class CarSalesTestCase {
public:
  typedef ParkingLot Request;
  typedef TotalValue Response;
  typedef uint64_t Expectation;

  static uint64_t setupRequest(ParkingLot* request) {
    uint count = fastRand(200);
    uint64_t result = 0;
    for (uint i = 0; i < count; i++) {
      Car* car = request->add_car();
      randomCar(car);
      result += carValue(*car);
    }
    return result;
  }
  static void handleRequest(const ParkingLot& request, TotalValue* response) {
    uint64_t result = 0;
    for (auto& car: request.car()) {
      result += carValue(car);
    }
    response->set_amount(result);
  }
  static inline bool checkResponse(const TotalValue& response, uint64_t expected) {
    return response.amount() == expected;
  }
};

// =======================================================================================

struct SingleUseMessages {
  template <typename MessageType>
  struct Message {
    struct Reusable {};
    struct SingleUse: public MessageType {
      inline SingleUse(Reusable&) {}
    };
  };

  struct ReusableString {};
  struct SingleUseString: std::string {
    inline SingleUseString(ReusableString&) {}
  };

  template <typename MessageType>
  static inline void doneWith(MessageType& message) {
    // Don't clear -- single-use.
  }
};

struct ReusableMessages {
  template <typename MessageType>
  struct Message {
    struct Reusable: public MessageType {};
    typedef MessageType& SingleUse;
  };

  typedef std::string ReusableString;
  typedef std::string& SingleUseString;

  template <typename MessageType>
  static inline void doneWith(MessageType& message) {
    message.Clear();
  }
};

// =======================================================================================
// The protobuf Java library defines a format for writing multiple protobufs to a stream, in which
// each message is prefixed by a varint size.  This was never added to the C++ library.  It's easy
// to do naively, but tricky to implement without accidentally losing various optimizations.  These
// two functions should be optimal.

struct Uncompressed {
  typedef google::protobuf::io::FileInputStream InputStream;
  typedef google::protobuf::io::FileOutputStream OutputStream;

  static uint64_t write(const google::protobuf::MessageLite& message,
                        google::protobuf::io::FileOutputStream* rawOutput) {
    google::protobuf::io::CodedOutputStream output(rawOutput);
    const int size = message.ByteSize();
    output.WriteVarint32(size);
    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != NULL) {
      message.SerializeWithCachedSizesToArray(buffer);
    } else {
      message.SerializeWithCachedSizes(&output);
      if (output.HadError()) {
        throw OsException(rawOutput->GetErrno());
      }
    }

    return size;
  }

  static void read(google::protobuf::io::ZeroCopyInputStream* rawInput,
                   google::protobuf::MessageLite* message) {
    google::protobuf::io::CodedInputStream input(rawInput);
    uint32_t size;
    GOOGLE_CHECK(input.ReadVarint32(&size));

    auto limit = input.PushLimit(size);

    GOOGLE_CHECK(message->MergePartialFromCodedStream(&input) &&
                 input.ConsumedEntireMessage());

    input.PopLimit(limit);
  }

  static void flush(google::protobuf::io::FileOutputStream* output) {
    if (!output->Flush()) throw OsException(output->GetErrno());
  }
};

// =======================================================================================
// The Snappy interface is really obnoxious.  I gave up here and am just reading/writing flat
// arrays in some static scratch space.  This probably gives protobufs an edge that it doesn't
// deserve.

static char scratch[1 << 20];
static char scratch2[1 << 20];

struct SnappyCompressed {
  typedef int InputStream;
  typedef int OutputStream;

  static uint64_t write(const google::protobuf::MessageLite& message, int* output) {
    size_t size = message.ByteSize();
    GOOGLE_CHECK_LE(size, sizeof(scratch));

    message.SerializeWithCachedSizesToArray(reinterpret_cast<uint8_t*>(scratch));

    size_t compressedSize = 0;
    snappy::RawCompress(scratch, size, scratch2 + sizeof(uint32_t), &compressedSize);
    uint32_t tag = compressedSize;
    memcpy(scratch2, &tag, sizeof(tag));

    writeAll(*output, scratch2, compressedSize + sizeof(tag));
    return compressedSize + sizeof(tag);
  }

  static void read(int* input, google::protobuf::MessageLite* message) {
    uint32_t size;
    readAll(*input, &size, sizeof(size));
    readAll(*input, scratch, size);

    size_t uncompressedSize;
    GOOGLE_CHECK(snappy::GetUncompressedLength(scratch, size, &uncompressedSize));
    GOOGLE_CHECK(snappy::RawUncompress(scratch, size, scratch2));

    GOOGLE_CHECK(message->ParsePartialFromArray(scratch2, uncompressedSize));
  }

  static void flush(OutputStream*) {}
};

// =======================================================================================

#define REUSABLE(type) \
  typename ReuseStrategy::template Message<typename TestCase::type>::Reusable
#define SINGLE_USE(type) \
  typename ReuseStrategy::template Message<typename TestCase::type>::SingleUse

template <typename TestCase, typename ReuseStrategy, typename Compression>
struct BenchmarkMethods {
  static uint64_t syncClient(int inputFd, int outputFd, uint64_t iters) {
    uint64_t throughput = 0;

    typename Compression::OutputStream output(outputFd);
    typename Compression::InputStream input(inputFd);

    REUSABLE(Request) reusableRequest;
    REUSABLE(Response) reusableResponse;

    for (; iters > 0; --iters) {
      SINGLE_USE(Request) request(reusableRequest);
      typename TestCase::Expectation expected = TestCase::setupRequest(&request);
      throughput += Compression::write(request, &output);
      Compression::flush(&output);
      ReuseStrategy::doneWith(request);

      SINGLE_USE(Response) response(reusableResponse);
      Compression::read(&input, &response);
      if (!TestCase::checkResponse(response, expected)) {
        throw std::logic_error("Incorrect response.");
      }
      ReuseStrategy::doneWith(response);
    }

    return throughput;
  }

  static uint64_t asyncClientSender(
      int outputFd, ProducerConsumerQueue<typename TestCase::Expectation>* expectations,
      uint64_t iters) {
    uint64_t throughput = 0;

    typename Compression::OutputStream output(outputFd);
    REUSABLE(Request) reusableRequest;

    for (; iters > 0; --iters) {
      SINGLE_USE(Request) request(reusableRequest);
      expectations->post(TestCase::setupRequest(&request));
      throughput += Compression::write(request, &output);
      Compression::flush(&output);
      ReuseStrategy::doneWith(request);
    }

    return throughput;
  }

  static void asyncClientReceiver(
      int inputFd, ProducerConsumerQueue<typename TestCase::Expectation>* expectations,
      uint64_t iters) {
    typename Compression::InputStream input(inputFd);
    REUSABLE(Response) reusableResponse;

    for (; iters > 0; --iters) {
      typename TestCase::Expectation expected = expectations->next();
      SINGLE_USE(Response) response(reusableResponse);
      Compression::read(&input, &response);
      if (!TestCase::checkResponse(response, expected)) {
        throw std::logic_error("Incorrect response.");
      }
      ReuseStrategy::doneWith(response);
    }
  }

  static uint64_t asyncClient(int inputFd, int outputFd, uint64_t iters) {
    ProducerConsumerQueue<typename TestCase::Expectation> expectations;
    std::thread receiverThread(asyncClientReceiver, inputFd, &expectations, iters);
    uint64_t throughput = asyncClientSender(outputFd, &expectations, iters);
    receiverThread.join();

    return throughput;
  }

  static uint64_t server(int inputFd, int outputFd, uint64_t iters) {
    uint64_t throughput = 0;

    typename Compression::OutputStream output(outputFd);
    typename Compression::InputStream input(inputFd);

    REUSABLE(Request) reusableRequest;
    REUSABLE(Response) reusableResponse;

    for (; iters > 0; --iters) {
      SINGLE_USE(Request) request(reusableRequest);
      Compression::read(&input, &request);

      SINGLE_USE(Response) response(reusableResponse);
      TestCase::handleRequest(request, &response);
      ReuseStrategy::doneWith(request);

      throughput += Compression::write(response, &output);
      Compression::flush(&output);
      ReuseStrategy::doneWith(response);
    }

    return throughput;
  }

  static uint64_t passByObject(uint64_t iters, bool countObjectSize) {
    uint64_t throughput = 0;

    REUSABLE(Request) reusableRequest;
    REUSABLE(Response) reusableResponse;

    for (; iters > 0; --iters) {
      SINGLE_USE(Request) request(reusableRequest);
      typename TestCase::Expectation expected = TestCase::setupRequest(&request);

      SINGLE_USE(Response) response(reusableResponse);
      TestCase::handleRequest(request, &response);
      ReuseStrategy::doneWith(request);
      if (!TestCase::checkResponse(response, expected)) {
        throw std::logic_error("Incorrect response.");
      }
      ReuseStrategy::doneWith(response);

      if (countObjectSize) {
        throughput += request.SpaceUsed();
        throughput += response.SpaceUsed();
      }
    }

    return throughput;
  }

  static uint64_t passByBytes(uint64_t iters) {
    uint64_t throughput = 0;

    REUSABLE(Request) reusableClientRequest;
    REUSABLE(Request) reusableServerRequest;
    REUSABLE(Response) reusableServerResponse;
    REUSABLE(Response) reusableClientResponse;
    typename ReuseStrategy::ReusableString reusableRequestString, reusableResponseString;

    for (; iters > 0; --iters) {
      SINGLE_USE(Request) clientRequest(reusableClientRequest);
      typename TestCase::Expectation expected = TestCase::setupRequest(&clientRequest);

      typename ReuseStrategy::SingleUseString requestString(reusableRequestString);
      clientRequest.SerializePartialToString(&requestString);
      throughput += requestString.size();
      ReuseStrategy::doneWith(clientRequest);

      SINGLE_USE(Request) serverRequest(reusableServerRequest);
      serverRequest.ParsePartialFromString(requestString);

      SINGLE_USE(Response) serverResponse(reusableServerResponse);
      TestCase::handleRequest(serverRequest, &serverResponse);
      ReuseStrategy::doneWith(serverRequest);

      typename ReuseStrategy::SingleUseString responseString(reusableResponseString);
      serverResponse.SerializePartialToString(&responseString);
      throughput += responseString.size();
      ReuseStrategy::doneWith(serverResponse);

      SINGLE_USE(Response) clientResponse(reusableClientResponse);
      clientResponse.ParsePartialFromString(responseString);

      if (!TestCase::checkResponse(clientResponse, expected)) {
        throw std::logic_error("Incorrect response.");
      }
      ReuseStrategy::doneWith(clientResponse);
    }

    return throughput;
  }
};

struct BenchmarkTypes {
  typedef protobuf::ExpressionTestCase ExpressionTestCase;
  typedef protobuf::CatRankTestCase CatRankTestCase;
  typedef protobuf::CarSalesTestCase CarSalesTestCase;

  typedef protobuf::SnappyCompressed SnappyCompressed;
  typedef protobuf::Uncompressed Uncompressed;

  typedef protobuf::ReusableMessages ReusableResources;
  typedef protobuf::SingleUseMessages SingleUseResources;

  template <typename TestCase, typename ReuseStrategy, typename Compression>
  struct BenchmarkMethods
      : public protobuf::BenchmarkMethods<TestCase, ReuseStrategy, Compression> {};
};

}  // namespace protobuf
}  // namespace benchmark
}  // namespace capnproto

int main(int argc, char* argv[]) {
  return capnproto::benchmark::benchmarkMain<
      capnproto::benchmark::protobuf::BenchmarkTypes>(argc, argv);
}
