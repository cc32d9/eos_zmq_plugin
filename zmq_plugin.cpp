/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 *  @author cc32d9 <cc32d9@gmail.com>
 */
#include <eosio/zmq_plugin/zmq_plugin.hpp>
#include <string>
#include <zmq.hpp>
#include <fc/io/json.hpp>

#include <eosio/chain/types.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/trace.hpp>
#include <eosio/chain_plugin/chain_plugin.hpp>

namespace {
  const char* SENDER_BIND = "zmq-sender-bind";
  const char* SENDER_BIND_DEFAULT = "tcp://127.0.0.1:5556";
}

namespace zmqplugin {
  using namespace eosio;
  using namespace eosio::chain;
  using account_resource_limit = chain::resource_limits::account_resource_limit;

  // these structures are not defined in contract_types.hpp, so we define them here
  namespace syscontract {

    struct buyrambytes {
      account_name payer;
      account_name receiver;
      uint32_t bytes;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(buyrambytes);}
    };

    struct buyram {
      account_name payer;
      account_name receiver;
      asset quant;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(buyram);}
    };

    struct sellram {
      account_name account;
      uint64_t bytes;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(sellram);}
    };

    struct delegatebw {
      account_name from;
      account_name receiver;
      asset stake_net_quantity;
      asset stake_cpu_quantity;
      bool transfer;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(delegatebw);}
    };

    struct undelegatebw {
      account_name from;
      account_name receiver;
      asset unstake_net_quantity;
      asset unstake_cpu_quantity;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(undelegatebw);}
    };

    struct refund {
      account_name owner;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(refund);}
    };

    struct regproducer {
      account_name producer;
      public_key_type producer_key;
      string url;
      uint16_t location;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(regproducer);}
    };

    struct unregprod {
      account_name producer;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(unregprod);}
    };

    struct regproxy {
      account_name proxy;
      bool isproxy;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(regproxy);}
    };

    struct voteproducer {
      account_name voter;
      account_name proxy;
      std::vector<account_name> producers;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(voteproducer);}
    };

    struct claimrewards {
      account_name owner;
      static account_name get_account() {return config::system_account_name;}
      static action_name get_name() {return N(claimrewards);}
    };
  }

  struct resource_balance {
    name                       account_name;
    int64_t                    ram_quota  = 0;
    int64_t                    ram_usage = 0;
    int64_t                    net_weight = 0;
    int64_t                    cpu_weight = 0;
    account_resource_limit     net_limit;
    account_resource_limit     cpu_limit;
  };

  struct currency_balance {
    name                       account_name;
    name                       issuer;
    asset                      balance;
  };

  struct zmq_action_object {
    uint64_t                     global_action_seq;
    block_num_type               block_num;
    chain::block_timestamp_type  block_time;
    fc::variant                  action_trace;
    vector<resource_balance>     resource_balances;
    vector<currency_balance>     currency_balances;
    uint32_t                     last_irreversible_block;
  };

  // see status definitions in libraries/chain/include/eosio/chain/block.hpp
  struct zmq_transaction_receipt {
    string                                         trx_id;
    eosio::chain::transaction_receipt::status_enum status;  // enum
    uint8_t                                        istatus; // the same as status, but integer
  };
  
  struct zmq_irreversible_block_object {
    block_num_type                    block_num;
    vector<zmq_transaction_receipt>   transactions;
  };
}


namespace eosio {
  using namespace chain;
  using namespace zmqplugin;
  using boost::signals2::scoped_connection;

  static appbase::abstract_plugin& _zmq_plugin = app().register_plugin<zmq_plugin>();

  class zmq_plugin_impl {
  public:
    zmq::context_t context;
    zmq::socket_t sender_socket;
    chain_plugin*          chain_plug = nullptr;
    fc::microseconds       abi_serializer_max_time;
    std::set<name>         system_accounts;
    std::map<name,std::set<name>>  blacklist_actions;

    fc::optional<scoped_connection> applied_transaction_connection;
    fc::optional<scoped_connection> irreversible_block_connection;

    zmq_plugin_impl():
      context(1),
      sender_socket(context, ZMQ_PUSH)
    {
      std::vector<name> sys_acc_names = {
        chain::config::system_account_name,
        N(eosio.msig),  N(eosio.token),  N(eosio.ram), N(eosio.ramfee),
        N(eosio.stake), N(eosio.vpay), N(eosio.bpay), N(eosio.saving)
      };

      for(name n : sys_acc_names) {
        system_accounts.insert(n);
      }

      blacklist_actions.emplace
        (std::make_pair(chain::config::system_account_name,
                        std::set<name>{ N(onblock) } ));
      blacklist_actions.emplace
        (std::make_pair(N(blocktwitter),
                        std::set<name>{ N(tweet) } ));
    }


    void send_msg( const string content, int32_t msgtype, int32_t msgopts)
    {
      zmq::message_t message(content.length()+sizeof(msgtype)+sizeof(msgopts));
      unsigned char* ptr = (unsigned char*) message.data();
      memcpy(ptr, &msgtype, sizeof(msgtype));
      ptr += sizeof(msgtype);
      memcpy(ptr, &msgopts, sizeof(msgopts));
      ptr += sizeof(msgopts);
      memcpy(ptr, content.c_str(), content.length());
      sender_socket.send(message);
    }


    void on_applied_transaction( const transaction_trace_ptr& trace )
    {
      // see a long comment in mongo_db_plugin.cpp for applied_transaction()
      if( !trace->producer_block_id.valid() ) {
        return;
      }

      for( const auto& atrace : trace->action_traces ) {
        on_action_trace( atrace );
      }
    }

    void on_action_trace( const action_trace& at )
    {
      // check the action against the blacklist
      auto search_acc = blacklist_actions.find(at.act.account);
      if(search_acc != blacklist_actions.end()) {
        auto search_act = search_acc->second.find(at.act.name);
        if( search_act != search_acc->second.end() ) {
          return;
        }
      }

      auto& chain = chain_plug->chain();

      zmq_action_object zao;
      zao.global_action_seq = at.receipt.global_sequence;
      zao.block_num = chain.pending_block_state()->block_num;
      zao.block_time = chain.pending_block_time();
      zao.action_trace = chain.to_variant_with_abi(at, abi_serializer_max_time);

      std::set<name> accounts;
      std::set<name> token_contracts;

      find_accounts_and_tokens(at, accounts, token_contracts);

      for (auto it = accounts.begin(); it != accounts.end(); ++it) {
        name account_name = *it;
        if( is_account_of_interest(account_name) ) {
          add_account_resource( zao, account_name );
          for (auto it2 = token_contracts.begin(); it2 != token_contracts.end(); ++it2) {
            add_currency_balances( zao, account_name, *it2 );
          }
        }
      }

      zao.last_irreversible_block = chain.last_irreversible_block_num();
      string zao_json = fc::json::to_string(zao);
      //idump((zao_json));
      send_msg(zao_json, 0, 0);
    }


    void on_irreversible_block( const chain::block_state_ptr& bs )
    {
      zmq_irreversible_block_object zibo;
      zibo.block_num = bs->block->block_num();

      for( const auto& receipt : bs->block->transactions ) {
        string trx_id_str;
        if( receipt.trx.contains<packed_transaction>() ) {
          const auto& pt = receipt.trx.get<packed_transaction>();
          // get id via get_raw_transaction() as packed_transaction.id() mutates internal transaction state
          const auto& raw = pt.get_raw_transaction();
          const auto& id = fc::raw::unpack<transaction>( raw ).id();
          trx_id_str = id.str();
        } else {
          const auto& id = receipt.trx.get<transaction_id_type>();
          trx_id_str = id.str();
        }

        zibo.transactions.emplace_back(zmq_transaction_receipt
                                       {trx_id_str, receipt.status, static_cast<uint8_t>(receipt.status)});
      }

      if( zibo.transactions.size() > 0 ) {
        string zibo_json = fc::json::to_string(zibo);
        //idump((zibo_json));
        send_msg(zibo_json, 1, 0);
      }
    }


    void find_accounts_and_tokens(const action_trace& at,
                                  std::set<name>& accounts,
                                  std::set<name>& token_contracts)
    {
      accounts.insert(at.act.account);

      if( at.receipt.receiver != at.act.account ) {
        accounts.insert(at.receipt.receiver);
      }

      if( at.act.account == config::system_account_name ) {
        switch((uint64_t) at.act.name) {
        case N(newaccount):
          {
            const auto data = at.act.data_as<chain::newaccount>();
            accounts.insert(data.name);
          }
          break;
        case N(setcode):
          {
            const auto data = at.act.data_as<chain::setcode>();
            accounts.insert(data.account);
          }
          break;
        case N(setabi):
          {
            const auto data = at.act.data_as<chain::setabi>();
            accounts.insert(data.account);
          }
          break;
        case N(updateauth):
          {
            const auto data = at.act.data_as<chain::updateauth>();
            accounts.insert(data.account);
          }
          break;
        case N(deleteauth):
          {
            const auto data = at.act.data_as<chain::deleteauth>();
            accounts.insert(data.account);
          }
          break;
        case N(linkauth):
          {
            const auto data = at.act.data_as<chain::linkauth>();
            accounts.insert(data.account);
          }
          break;
        case N(unlinkauth):
          {
            const auto data = at.act.data_as<chain::unlinkauth>();
            accounts.insert(data.account);
          }
          break;
        case N(buyrambytes):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::buyrambytes>();
            accounts.insert(data.payer);
            if( data.receiver != data.payer ) {
              accounts.insert(data.receiver);
            }
          }
          break;
        case N(buyram):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::buyram>();
            accounts.insert(data.payer);
            if( data.receiver != data.payer ) {
              accounts.insert(data.receiver);
            }
          }
          break;
        case N(sellram):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::sellram>();
            accounts.insert(data.account);
          }
          break;
        case N(delegatebw):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::delegatebw>();
            accounts.insert(data.from);
            if( data.receiver != data.from ) {
              accounts.insert(data.receiver);
            }
          }
          break;
        case N(undelegatebw):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::undelegatebw>();
            accounts.insert(data.from);
            if( data.receiver != data.from ) {
              accounts.insert(data.receiver);
            }
          }
          break;
        case N(refund):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::refund>();
            accounts.insert(data.owner);
          }
          break;
        case N(regproducer):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::regproducer>();
            accounts.insert(data.producer);
          }
          break;
        case N(bidname):
          {
            // do nothing because newname account does not exist yet
          }
          break;
        case N(unregprod):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::unregprod>();
            accounts.insert(data.producer);
          }
          break;
        case N(regproxy):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::regproxy>();
            accounts.insert(data.proxy);
          }
          break;
        case N(voteproducer):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::voteproducer>();
            accounts.insert(data.voter);
            if( data.proxy ) {
              accounts.insert(data.proxy);
            }
            // not including the producrs list, although some projects may need it
          }
          break;
        case N(claimrewards):
          {
            const auto data = at.act.data_as<zmqplugin::syscontract::claimrewards>();
            accounts.insert(data.owner);
          }
          break;
        }
      }
      else if( at.act.name == N(issue) || at.act.name == N(transfer) ) {
        token_contracts.insert(at.act.account);
      }

      for( const auto& iline : at.inline_traces ) {
        find_accounts_and_tokens( iline, accounts, token_contracts );
      }
    }

    bool is_account_of_interest(name account_name)
    {
      auto search = system_accounts.find(account_name);
      if(search != system_accounts.end()) {
        return false;
      }
      return true;
    }

    void add_account_resource( zmq_action_object& zao, name account_name )
    {
      resource_balance bal;
      bal.account_name = account_name;

      auto& chain = chain_plug->chain();
      const auto& rm = chain.get_resource_limits_manager();

      rm.get_account_limits( account_name, bal.ram_quota, bal.net_weight, bal.cpu_weight );
      bool grelisted = chain.is_resource_greylisted(account_name);
      bal.net_limit = rm.get_account_net_limit_ex( account_name, !grelisted);
      bal.cpu_limit = rm.get_account_cpu_limit_ex( account_name, !grelisted);
      bal.ram_usage = rm.get_account_ram_usage( account_name );
      zao.resource_balances.emplace_back(bal);
    }

    void add_currency_balances( zmq_action_object& zao,
                                name account_name, name token_code )
    {
      const auto& chain = chain_plug->chain();
      const auto& db = chain.db();

      const auto* t_id = db.find<chain::table_id_object, chain::by_code_scope_table>
        (boost::make_tuple( token_code, account_name, N(accounts) ));
      if( t_id != nullptr ) {
          const auto &idx = db.get_index<key_value_index, by_scope_primary>();
          decltype(t_id->id) next_tid(t_id->id._id + 1);
          auto lower = idx.lower_bound(boost::make_tuple(t_id->id));
          auto upper = idx.lower_bound(boost::make_tuple(next_tid));

          for (auto obj = lower; obj != upper; ++obj) {
            if( obj->value.size() >= sizeof(asset) ) {
              asset bal;
              fc::datastream<const char *> ds(obj->value.data(), obj->value.size());
              fc::raw::unpack(ds, bal);
              if( bal.get_symbol().valid() ) {
                zao.currency_balances.emplace_back(currency_balance{account_name, token_code, bal});
              }
            }
          }
      }
    }
  };


  zmq_plugin::zmq_plugin():my(new zmq_plugin_impl()){}
  zmq_plugin::~zmq_plugin(){}

  void zmq_plugin::set_program_options(options_description&, options_description& cfg)
  {
    cfg.add_options()
      (SENDER_BIND, bpo::value<string>()->default_value(SENDER_BIND_DEFAULT),
       "ZMQ Sender Socket binding")
      ;
  }

  void zmq_plugin::plugin_initialize(const variables_map& options)
  {
    string bind_str = options.at(SENDER_BIND).as<string>();
    if (bind_str.empty()) {
      wlog("zmq-sender-bind not specified => eosio::zmq_plugin disabled.");
      return;
    }

    ilog("Binding to ZMQ PUSH socket ${u}", ("u", bind_str));
    my->sender_socket.bind(bind_str);

    my->chain_plug = app().find_plugin<chain_plugin>();
    my->abi_serializer_max_time = my->chain_plug->get_abi_serializer_max_time();

    auto& chain = my->chain_plug->chain();
    my->applied_transaction_connection.emplace
      ( chain.applied_transaction.connect( [&]( const transaction_trace_ptr& p ){
          my->on_applied_transaction(p);  }));
    my->irreversible_block_connection.emplace
      ( chain.irreversible_block.connect( [&]( const chain::block_state_ptr& bs ) {
          my->on_irreversible_block( bs ); } ));
  }

  void zmq_plugin::plugin_startup() {

  }

  void zmq_plugin::plugin_shutdown() {
  }

}

FC_REFLECT( zmqplugin::syscontract::buyrambytes,
            (payer)(receiver)(bytes) )

FC_REFLECT( zmqplugin::syscontract::buyram,
            (payer)(receiver)(quant) )

FC_REFLECT( zmqplugin::syscontract::sellram,
            (account)(bytes) )

FC_REFLECT( zmqplugin::syscontract::delegatebw,
            (from)(receiver)(stake_net_quantity)(stake_cpu_quantity)(transfer) )

FC_REFLECT( zmqplugin::syscontract::undelegatebw,
            (from)(receiver)(unstake_net_quantity)(unstake_cpu_quantity) )

FC_REFLECT( zmqplugin::syscontract::refund,
            (owner) )

FC_REFLECT( zmqplugin::syscontract::regproducer,
            (producer)(producer_key)(url)(location) )

FC_REFLECT( zmqplugin::syscontract::unregprod,
            (producer) )

FC_REFLECT( zmqplugin::syscontract::regproxy,
            (proxy)(isproxy) )

FC_REFLECT( zmqplugin::syscontract::voteproducer,
            (voter)(proxy)(producers) )

FC_REFLECT( zmqplugin::syscontract::claimrewards,
            (owner) )

FC_REFLECT( zmqplugin::resource_balance,
            (account_name)(ram_quota)(ram_usage)(net_weight)(cpu_weight)(net_limit)(cpu_limit) )

FC_REFLECT( zmqplugin::currency_balance,
            (account_name)(issuer)(balance))

FC_REFLECT( zmqplugin::zmq_action_object,
            (global_action_seq)(block_num)(block_time)(action_trace)
            (resource_balances)(currency_balances)(last_irreversible_block) )

FC_REFLECT( zmqplugin::zmq_transaction_receipt,
            (trx_id)(status)(istatus) )

FC_REFLECT( zmqplugin::zmq_irreversible_block_object,
            (block_num)(transactions) )
