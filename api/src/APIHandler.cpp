#include <APIHandler.h>
#include <DebugLog.h>

#include "csconnector/csconnector.h"

#include <csdb/address.h>
#include <csdb/amount.h>
#include <csdb/csdb.h>
#include <csdb/currency.h>
#include <csdb/pool.h>
#include <csdb/storage.h>
#include <csdb/transaction.h>
#include <csdb/wallet.h>

#include <algorithm>
#include <cassert>

#include <api_types.h>

#include <net/Logger.hpp>

#include <iomanip>

#include <boost/io/ios_state.hpp>

using namespace api;

APIHandler::APIHandler(Credits::BlockChain& blockchain,
                       Credits::ISolver& _solver)
  : s_blockchain(blockchain)
  , solver(_solver)
  , stats(blockchain)
  , executor_transport(new thrift::transport::TBufferedTransport(
      thrift::stdcxx::make_shared<thrift::transport::TSocket>("localhost",
                                                              9080)))
  , executor(thrift::stdcxx::make_shared<thrift::protocol::TBinaryProtocol>(
      executor_transport))
{
    std::cerr << (s_blockchain.isGood() ? "Storage is opened normal"
                                        : "Storage is not opened")
              << std::endl;
    if (!s_blockchain.isGood()) {
        return;
    }
    update_smart_caches();
}

void
APIHandlerBase::SetResponseStatus(APIResponse& response,
                                  APIRequestStatusType status,
                                  const std::string& details)
{
    struct APIRequestStatus
    {
        APIRequestStatus(uint8_t code, std::string message)
          : message(message)
          , code(code)
        {}
        std::string message;
        uint8_t code;
    };

    APIRequestStatus statuses[static_cast<size_t>(
      APIHandlerBase::APIRequestStatusType::MAX)] = {
        {
          0,
          "Success",
        },
        {
          1,
          "Failure",
        },
        { 2, "Not Implemented" },
    };
    response.code = statuses[static_cast<uint8_t>(status)].code;
    response.message = statuses[static_cast<uint8_t>(status)].message + details;
}

void
APIHandlerBase::SetResponseStatus(APIResponse& response, bool commandWasHandled)
{
    SetResponseStatus(response,
                      (commandWasHandled
                         ? APIRequestStatusType::SUCCESS
                         : APIRequestStatusType::NOT_IMPLEMENTED));
}

void
APIHandler::BalanceGet(BalanceGetResult& _return,
                       const Address& address,
                       const Currency& currency)
{
    csdb::Address addr;
    if (address.size() != 64)
        addr = Credits::BlockChain::getAddressFromKey(address.c_str());
    else
        addr = csdb::Address::from_string(address);

    auto result = s_blockchain.getBalance(addr);
    _return.amount.integral = result.integral();
    _return.amount.fraction = result.fraction();

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

struct TransID
{
    std::string PoolHash;
    size_t index;
};

void
convertStringToID(const std::string data, TransID& id)
{
    id.index = 0;
    id.PoolHash = "";

    const short hash_size = 64;
    id.PoolHash = data.substr(0, hash_size);

    if (data.size() > hash_size + 1) {
        std::string string_index =
          data.substr(hash_size + 1, data.size() - (hash_size + 1));

        id.index = std::stoi(string_index);
    }
}

api::Amount
convertAmount(const csdb::Amount& amount)
{
    api::Amount result;
    result.integral = amount.integral();
    result.fraction = amount.fraction();
    assert(result.fraction >= 0);
    return result;
}

api::Transaction
convertTransaction(const csdb::Transaction& transaction)
{
    api::Transaction result;
    const csdb::Amount& amount = transaction.amount();
    const csdb::Currency& currency = transaction.currency();
    const csdb::Address& target = transaction.target();
    const csdb::TransactionID& id = transaction.id();
    const csdb::Address& address = transaction.source();

    result.amount = convertAmount(amount);
    result.currency = currency.to_string();
    result.innerId = id.to_string();
    result.source = address.to_string();
    result.target = target.to_string();

    return result;
}

api::Transactions
convertTransactions(const std::vector<csdb::Transaction>& transactions)
{
    api::Transactions result;
    // reserve vs resize
    result.resize(transactions.size());
    std::transform(transactions.begin(),
                   transactions.end(),
                   result.begin(),
                   convertTransaction);
    return result;
}

api::Pool
APIHandler::convertPool(const csdb::Pool& pool)
{
    api::Pool result;
    pool.is_valid();
    if (pool.is_valid()) {
        result.hash = pool.hash().to_string();
        result.poolNumber = pool.sequence();
        assert(result.poolNumber >= 0);
        result.prevHash = pool.previous_hash().to_string();
        std::cerr << pool.user_field(0).value<std::string>() << std::endl;
        result.time = atoll(
          pool.user_field(0)
            .value<std::string>()
            .c_str()); 

        result.transactionsCount =
          (int32_t)pool.transactions_count(); 
                                              
    }
    return result;
}

api::Pool
APIHandler::convertPool(const csdb::PoolHash& poolHash)
{
    const csdb::Pool& pool = s_blockchain.loadBlock(poolHash);
    return convertPool(pool);
}

api::Transactions
extractTransactions(const csdb::Pool& pool, int64_t limit, const int64_t offset)
{
    int64_t transactionsCount = pool.transactions_count();
    assert(transactionsCount >= 0);

    if (offset > transactionsCount) {
        return api::Transactions{}; 
                                    
    }
    api::Transactions result;
    transactionsCount -=
      offset; 

    if (limit > transactionsCount)
        limit = transactionsCount; 
                                   

    for (int64_t index = offset; index < (offset + limit); ++index) {
        const csdb::Transaction transaction = pool.transaction(index);
        result.push_back(convertTransaction(transaction));
    }
    return result;
}

void
APIHandler::TransactionGet(TransactionGetResult& _return,
                           const TransactionId& transactionId)
{
    Log("TransactionGet");

    TransID id;
    convertStringToID(transactionId, id);

    const csdb::PoolHash& poolhash = csdb::PoolHash::from_string(id.PoolHash);
    const csdb::TransactionID& tmpTransactionId =
      csdb::TransactionID(poolhash, (id.index));
    const csdb::Transaction& transaction =
      s_blockchain.loadTransaction(tmpTransactionId);

    _return.found = transaction.is_valid();
    if (_return.found) {
        _return.transaction = convertTransaction(transaction);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void
APIHandler::TransactionsGet(TransactionsGetResult& _return,
                            const Address& address,
                            int64_t offset,
                            const int64_t limit)
{
    Log("TransactionsGet");

    std::string tmp_buff;

    csdb::Address addr;
    if (address.size() != 64)
        addr = Credits::BlockChain::getAddressFromKey(address.c_str());
    else
        addr = csdb::Address::from_string(address);

    std::vector<csdb::Transaction> transactions;
    csdb::Pool curr = s_blockchain.loadBlock(s_blockchain.getLastHash());

    while (curr.is_valid()) {
        if (curr.transactions_count()) {
            auto curIdx = static_cast<csdb::TransactionID::sequence_t>(
              curr.transactions_count() - 1);

            while (true) {
                auto trans = curr.transaction(curIdx);
                if (trans.target() == addr || trans.source() == addr) {
                    if (offset == 0)
                        transactions.push_back(trans);
                    else
                        --offset;
                }

                if (transactions.size() == limit)
                    break;

                if (curIdx == 0)
                    break;
                --curIdx;
            }
        }

        if (transactions.size() == limit)
            break;
        curr = s_blockchain.loadBlock(curr.previous_hash());
    }

    _return.transactions = convertTransactions(transactions);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

std::string
serialize(const api::SmartContract& sc)
{
    auto buffer = stdcxx::make_shared<TMemoryBuffer>();
    TBinaryProtocol proto(buffer);
    sc.write(&proto);
    return buffer->getBufferAsString();
}

api::SmartContract
deserialize(std::string& s)
{
    
    static_assert(
      CHAR_BIT == 8 && std::is_same_v<std::uint8_t, unsigned char>,
      "This code requires std::uint8_t to be implemented as unsigned char.");

    auto buffer = stdcxx::make_shared<TMemoryBuffer>(
      reinterpret_cast<uint8_t*>(&(s[0])), (uint32_t)s.size());
    SUPER_TIC();
    TBinaryProtocol proto(buffer);
    SUPER_TIC();
    api::SmartContract sc;
    SUPER_TIC();
    sc.read(&proto);
    SUPER_TIC();
    return sc;
}

api::SmartContract
fetch_smart(const csdb::Transaction& tr)
{
    return tr.is_valid() ? deserialize(tr.user_field(0).value<std::string>()) : api::SmartContract();
}

bool
is_smart(const csdb::Transaction& tr)
{
    csdb::UserField uf = tr.user_field(0);
    return uf.type() == csdb::UserField::Type::String;
}

bool
is_smart_deploy(const api::SmartContract& smart)
{
    return !smart.byteCode.empty();
}

class ToHex
{
    const std::string& s;

  public:
    ToHex(const std::string& s)
      : s(s)
    {}

    friend std::ostream& operator<<(std::ostream& os, const ToHex& th);
};


void
APIHandler::TransactionFlow(TransactionFlowResult& _return,
                            const Transaction& transaction)
{


    if (transaction.target == "accXpfvxnZa8txuxpjyPqzBaqYPHqYu2rwn34lL8rjI=") {
        
        return;
    }


    SUPER_TIC();

    csdb::Transaction send_transaction;
    PublicKey from, to;

    auto source =
      Credits::BlockChain::getAddressFromKey(transaction.source.c_str());

    const uint64_t WALLET_DENOM = 1'000'000'000'000'000'000ull;

    send_transaction.set_amount(csdb::Amount(
      transaction.amount.integral, transaction.amount.fraction, WALLET_DENOM));
    send_transaction.set_balance(s_blockchain.getBalance(source));
    send_transaction.set_currency(csdb::Currency("CS"));
    send_transaction.set_source(source);
    send_transaction.set_target(
      Credits::BlockChain::getAddressFromKey(transaction.target.c_str()));

    SUPER_TIC();

    bool smart = !transaction.smartContract.address.empty();

    api::SmartContract smart_for_executor, smart_for_net;

    if (smart) {
        update_smart_caches();
        SUPER_TIC();

        smart_for_executor = fetch_smart(
          s_blockchain.loadTransaction(smart_origin[transaction.target]));

        SUPER_TIC();

        smart_for_net = fetch_smart(
          s_blockchain.loadTransaction(smart_state[transaction.target]));

        SUPER_TIC();

        auto input_smart = transaction.smartContract;
        if (!input_smart.method.empty()) {
            input_smart.byteCode = std::string();
            input_smart.sourceCode = std::string();
        }
        bool deploy = is_smart_deploy(input_smart);


        if (smart_for_executor.address.empty() == deploy) {
            SUPER_TIC();
            if (deploy) {
                smart_for_net.byteCode = input_smart.byteCode;
                smart_for_net.sourceCode = input_smart.sourceCode;
            }
            smart_for_net.address = transaction.target;
            smart_for_net.hashState = input_smart.hashState;
            smart_for_net.method = input_smart.method;
            smart_for_net.params = input_smart.params;
        } else {
            SUPER_TIC();
            SetResponseStatus(_return.status, APIRequestStatusType::FAILURE);
            return;
        }
    }

    SUPER_TIC();

    if (smart) {
        send_transaction.add_user_field(0, serialize(smart_for_net));
        send_transaction.set_amount(csdb::Amount(1));
    }

    SUPER_TIC();

    if (&solver == nullptr) {
        LOG_ERROR("solver == nullptr");
        return;
    }
    solver.send_wallet_transaction(send_transaction);

    SUPER_TIC();

    if (!smart) {
        SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
        return;
    }

    SUPER_TIC();

    executor::APIResponse api_resp;

    SUPER_TIC();

    static auto transport_opened = false;
    if (!transport_opened) {
        transport_opened = true;
        executor_transport->open();
    }

    SUPER_TIC();

    executor.executeByteCode(
      api_resp,
      smart_for_net.address,
      smart_for_executor.byteCode,
      smart_for_net.contractState,
      (smart_for_net.method.empty() ? "initialize" : smart_for_net.method),
      smart_for_net.params);

    SUPER_TIC();

    api::SmartContract new_smart;
    new_smart.contractState = api_resp.contractState;
    new_smart.method = smart_for_net.method;
    new_smart.params = smart_for_net.params;
    new_smart.address = smart_for_net.address;

    csdb::Transaction contract_redeploy_tr = send_transaction;
    contract_redeploy_tr.add_user_field(0, serialize(new_smart));
    solver.send_wallet_transaction(contract_redeploy_tr);

    SUPER_TIC();

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
    return;
}

void
APIHandler::PoolListGet(api::PoolListGetResult& _return,
            const int64_t offset,
            const int64_t const_limit)
{

    if (offset > 100)
        const_cast<int64_t&>(offset) = 100;
    if (const_limit > 100)
        const_cast<int64_t&>(const_limit) = 100;

    _return.pools.reserve(const_limit);

    csdb::PoolHash hash = s_blockchain.getLastHash();

    size_t lastCount = 0;
    csdb::Pool pool; 
    uint64_t sequence = s_blockchain.getSize();

    const uint64_t lower =
      sequence - std::min(sequence, (uint64_t)(offset + const_limit));
    for (uint64_t it = sequence; it > lower; --it) {
        auto cch = poolCache.find(hash);

        if (cch == poolCache.end()) {
            pool = s_blockchain.loadBlock(hash);
            api::Pool apiPool = convertPool(pool);

            if (it <= sequence - std::min(sequence, (uint64_t)offset)) {
                _return.pools.push_back(apiPool);
            }
            lastCount = 0;

            poolCache.insert(cch, std::make_pair(hash, apiPool));
            hash = pool.previous_hash();
        } else {
            _return.pools.push_back(cch->second);
            hash = csdb::PoolHash::from_string(cch->second.prevHash);
        }
    }
}

void
APIHandler::PoolTransactionsGet(PoolTransactionsGetResult& _return,
                                const PoolHash& hash,
                                const int64_t index,
                                const int64_t offset,
                                const int64_t limit)
{
    Log("PoolTransactionsGet");
    const csdb::PoolHash poolHash = csdb::PoolHash::from_string(hash);
    const csdb::Pool& pool = s_blockchain.loadBlock(poolHash);

    if (pool.is_valid()) {
        _return.transactions = extractTransactions(pool, limit, offset);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void
APIHandler::PoolInfoGet(PoolInfoGetResult& _return,
                        const PoolHash& hash,
                        const int64_t index)
{
    Log("PoolInfoGet");

    const csdb::PoolHash poolHash = csdb::PoolHash::from_string(hash);
    const csdb::Pool pool = s_blockchain.loadBlock(poolHash);
    _return.isFound = pool.is_valid();

    if (_return.isFound) {
        _return.pool = convertPool(poolHash);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void
APIHandler::StatsGet(api::StatsGetResult& _return)
{
    Log("StatsGet");

    csstats::StatsPerPeriod stats = this->stats.getStats();

    for (auto& s : stats) {
        api::PeriodStats ps = {};
        ps.periodDuration = s.periodSec;
        ps.poolsCount = s.poolsCount;
        ps.transactionsCount = s.transactionsCount;
        ps.smartContractsCount = s.smartContractsCount;

        for (auto& t : s.balancePerCurrency) {
            api::CumulativeAmount amount;
            amount.integral = t.second.integral;
            amount.fraction = t.second.fraction;
            ps.balancePerCurrency[t.first] = amount;
        }

        _return.stats.push_back(ps);
    }

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}

void
APIHandler::NodesInfoGet(api::NodesInfoGetResult& _return)
{
    Log("NodesInfoGet");

    SetResponseStatus(_return.status, APIRequestStatusType::NOT_IMPLEMENTED);
}


void
APIHandler::SmartContractGet(api::SmartContractGetResult& _return,
                             const api::Address& address)
{
    Log("SmartContractGet");


    update_smart_caches();

    auto trid = smart_origin[address];
    auto tr = s_blockchain.loadTransaction(trid);
    _return.smartContract = fetch_smart(tr);

    SetResponseStatus(_return.status,
                      !_return.smartContract.address.empty()
                        ? APIRequestStatusType::SUCCESS
                        : APIRequestStatusType::FAILURE);
    return;
}

void
APIHandler::update_smart_caches()
{
    std::map<csdb::Address, std::list<csdb::TransactionID>::iterator> poss;
    std::set<api::Address> state_updated;
    auto last_ph = s_blockchain.getLastHash();
    auto curr_ph = last_ph;
    while (curr_ph != last_seen_contract_block) {
        auto p = s_blockchain.loadBlock(curr_ph);
        auto&& trs = p.transactions();
        for (auto i_tr = trs.rbegin(); i_tr != trs.rend(); ++i_tr) {
            auto&& tr = *i_tr;
            if (!is_smart(tr)) {
                continue;
            }
            SUPER_TIC();
            auto smart = fetch_smart(tr);
            if (is_smart_deploy(smart)) {
                smart_origin[smart.address] = tr.id();
                SUPER_TIC();
                auto& targetList = deployed_by_creator[tr.source()];
                auto p = poss.find(tr.source());
                if (p == poss.end())
                    p = poss.insert(
                      p, std::make_pair(tr.source(), targetList.begin()));
                SUPER_TIC();
                targetList.insert(p->second, tr.id());
                SUPER_TIC();
            } else if (!state_updated.count(smart.address)) {
                SUPER_TIC();
                smart_state[smart.address] = tr.id();
                state_updated.insert(smart.address);
            }
        }
        curr_ph = p.previous_hash();
    }
    last_seen_contract_block = last_ph;
}

template<typename Mapper>
void
APIHandler::get_mapped_deployer_smart(
  const csdb::Address& deployer,
  Mapper mapper,
  std::vector<decltype(mapper(api::SmartContract()))>& out)
{
    update_smart_caches();

    for (auto& trid : deployed_by_creator[deployer]) {
        auto tr = s_blockchain.loadTransaction(trid);
        auto smart = fetch_smart(tr);
        out.push_back(mapper(smart));
    }
}

void
APIHandler::SmartContractsListGet(api::SmartContractsListGetResult& _return,
                                  const api::Address& deployer)
{
    Log("SmartContractsListGet");

    csdb::Address addr =
      Credits::BlockChain::getAddressFromKey(deployer.c_str());

    get_mapped_deployer_smart(
      addr,
      [](const api::SmartContract& smart) { return smart; },
      _return.smartContractsList);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);

}

void
APIHandler::SmartContractAddressesListGet(
  api::SmartContractAddressesListGetResult& _return,
  const api::Address& deployer)
{
    Log("SmartContractAddressesListGet");


    csdb::Address addr =
      Credits::BlockChain::getAddressFromKey(deployer.c_str());

    get_mapped_deployer_smart(
      addr,
      [](const SmartContract& sc) { return sc.address; },
      _return.addressesList);

    SetResponseStatus(_return.status, APIRequestStatusType::SUCCESS);
}