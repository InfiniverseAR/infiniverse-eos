#include "infiniverse.hpp"
#include "lat_long_functions.cpp"
#include <eosiolib/transaction.hpp>

const uint32_t seconds_in_one_day = 60 * 60 * 24;
const uint32_t seconds_in_one_week = seconds_in_one_day * 7;
const uint32_t seconds_in_one_year = seconds_in_one_day * 365;
const uint32_t max_land_length = 1000;
const symbol inf_symbol = symbol("INF", 4);
const name inf_account = "infinicoinio"_n;
const uint32_t reg_inf_per_sqm = 1;

void infiniverse::registerland(name owner, double lat_north_edge,
    double long_east_edge, double lat_south_edge, double long_west_edge)
{
    require_auth(owner);

    /*auction_table auctions(_self, _self.value);
    auction land_auction = auctions.get(0, "The land auction has not started");
    time_point_sec start_date = land_auction.start_date;
    time_point_sec end_date = start_date + seconds_in_one_week;
    time_point_sec current_date = time_point_sec(now());
    eosio_assert(current_date >= end_date, "You cannot register land until the auction is complete");*/

    std::pair<double, double> land_size = assert_lat_long_values(lat_north_edge, long_east_edge,
        lat_south_edge, long_west_edge);

    land_table lands(_self, _self.value);
    auto lat_north_index = lands.get_index<"bylatnorth"_n>();
    auto lands_itr = lat_north_index.lower_bound(lat_south_edge);
    // Add max_land_length meters to lat_north_edge to get upper bound
    double upper_bound = lat_north_edge + meters_to_lat_dist(max_land_length);

    asset inf_amount = asset(0, inf_symbol);

    while(lands_itr != lat_north_index.end() && lands_itr->lat_north_edge < upper_bound)
    {
        land l = *lands_itr;
        // If an intersecing land is completely inside (covered by) the new land
        // it must be owned by the same user
        if(l.lat_north_edge <= lat_north_edge &&
            l.lat_south_edge >= lat_south_edge &&
            l.long_east_edge <= long_east_edge &&
            l.long_west_edge >= long_west_edge)
        {
            eosio_assert(owner == l.owner, "Intersecting land has already been registered");

            std::pair<double, double> covered_land_size = lat_long_to_meters(
                l.lat_north_edge, l.long_east_edge, l.lat_south_edge, l.long_west_edge);
            
            double covered_land_area = covered_land_size.first * covered_land_size.second;
            double reg_years_left = (l.reg_end_date.utc_seconds - now()) / (double)seconds_in_one_year;
            inf_amount -= calculate_land_reg_fee(covered_land_area, reg_inf_per_sqm, reg_years_left);

            // Erase the covered land as it has been replaced
            lands_itr = lat_north_index.erase(lands_itr);
        }
        // Otherwise, any other kind of intersection is not allowed
        else
        {
            eosio_assert(
                l.long_east_edge <= long_west_edge ||
                l.long_west_edge >= long_east_edge ||
                l.lat_south_edge >= lat_north_edge ||
                // Required because lower bound includes equality case
                l.lat_north_edge <= lat_south_edge,
                "Intersecting land has already been registered");
            lands_itr++;
        }
    }

    landbid_table landbids(_self, _self.value);
    check_landbid_intersections(landbids, lat_north_edge, long_east_edge, lat_south_edge, long_west_edge);

    inf_amount += calculate_land_reg_fee(land_size, reg_inf_per_sqm, 1);
    if(inf_amount.amount < 1 * 10000)
    {
        inf_amount = asset(1 * 10000, inf_symbol);
    }
    deduct_inf_deposit(owner, inf_amount);

    // The registration fee gets sent back to the token issuing account
    transfer_inf(_self, inf_account, inf_amount, "Land registration fee");

    lands.emplace(owner, [&](auto &row) {
        row.id = lands.available_primary_key();
        row.owner = owner;
        row.lat_north_edge = lat_north_edge;
        row.long_east_edge = long_east_edge;
        row.lat_south_edge = lat_south_edge;
        row.long_west_edge = long_west_edge;
        row.reg_end_date = time_point_sec(now() + seconds_in_one_year);
    });
}

void infiniverse::persistpoly(uint64_t land_id, std::string poly_id,
        vector3 position, vector3 orientation, vector3 scale)
{
    name user = require_land_owner_auth(land_id);
    assert_vectors_within_bounds(position, orientation, scale);

    uint64_t source = static_cast<uint64_t>(PlacementSource::POLY);
    uint64_t asset_id = add_poly(user, poly_id);

    persistent_table persistents(_self, _self.value);
    persistents.emplace(user, [&](auto &row) {
        row.id = persistents.available_primary_key();
        row.land_id = land_id;
        row.source = source;
        row.asset_id = asset_id;
        row.position = position;
        row.orientation = orientation;
        row.scale = scale;
    });
}

void infiniverse::updatepersis(uint64_t persistent_id, uint64_t land_id,
    vector3 position, vector3 orientation, vector3 scale)
{
    persistent_table persistents(_self, _self.value);
    uint64_t old_land_id = get_land_id_from_persistent(persistents, persistent_id);
    require_land_owner_auth(old_land_id);
    if(land_id != old_land_id)
    {
        require_land_owner_auth(land_id);
    }
    assert_vectors_within_bounds(position, orientation, scale);
    
    auto persistents_itr = persistents.find(persistent_id);
    persistents.modify(persistents_itr, same_payer, [&](auto &row) {
        row.land_id = land_id;
        row.position = position;
        row.orientation = orientation;
        row.scale = scale;
    });
}

void infiniverse::deletepersis(uint64_t persistent_id)
{
    persistent_table persistents(_self, _self.value);
    uint64_t land_id = get_land_id_from_persistent(persistents, persistent_id);
    name user = require_land_owner_auth(land_id);

    persistent p = persistents.get(persistent_id);
    uint64_t source = p.source;
    uint64_t asset_id = p.asset_id;
    uint128_t source_and_asset_id = p.get_source_and_asset_id();
    persistents.erase(persistents.find(persistent_id));

    // If this is a poly asset, we can delete it if the user has not placed it elsewhere
    if(static_cast<PlacementSource>(source) == PlacementSource::POLY)
    {
        auto asset_id_index = persistents.get_index<"byassetid"_n>();
        // Asset id is unique per user even if it's the same poly id
        // Otherwise it would not be clear who should pay for the RAM of a poly object
        auto itr = asset_id_index.find(source_and_asset_id);
        if(itr == asset_id_index.end())
        {
            poly_table poly(_self, _self.value);
            auto poly_itr = poly.find(asset_id);
            poly.erase(poly_itr);
        }
    }

}

void infiniverse::opendeposit(name owner)
{
    require_auth(owner);
    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(owner.value);
    if(deposits_itr == deposits.end())
    {
        deposits.emplace(owner, [&](auto &row) {
            row.owner = owner;
            row.balance = asset(0, inf_symbol);
        });
    }
}

void infiniverse::closedeposit(name owner)
{
    eosio_assert(has_auth(owner) || has_auth(_self), "You do not have authority to close this deposit");
    deposit_table deposits(_self, _self.value);
    deposit d = deposits.get(owner.value, "User does not have a deposit opened");

    if(d.balance.amount > 0)
    {
        transfer_inf(_self, owner, d.balance, "INF deposit refund");
    }

    deposits.erase(deposits.find(owner.value));
}

void infiniverse::depositinf(name from, name to, asset quantity, std::string memo)
{
    // In case the tokens are from us, or not to us, do nothing
    if(from == _self || to != _self)
        return;
    // This should never happen as we ensured transfer action belongs to "infinicoinio" account
    eosio_assert(quantity.symbol == inf_symbol, "The symbol does not match");
    eosio_assert(quantity.is_valid(), "The quantity is not valid");
    eosio_assert(quantity.amount > 0, "The amount must be positive");

    deposit_table deposits(_self, _self.value);
    auto deposits_itr = deposits.find(from.value);
    eosio_assert(deposits_itr != deposits.end(), "User does not have a deposit opened");

    deposits.modify(deposits_itr, same_payer, [&](auto &row){
        row.balance += quantity;
    });
}

/*void infiniverse::startauction()
{
    require_auth(_self);
    auction_table auctions(_self, _self.value);
    auto auctions_itr = auctions.find(0);
    eosio_assert(auctions_itr == auctions.end(), "The land auction has already started");

    auctions.emplace(_self, [&](auto &row) {
        row.id = 0;
        row.start_date = time_point_sec(now());
    });
}*/

void infiniverse::makelandbid(name owner, double lat_north_edge, double long_east_edge,
    double lat_south_edge, double long_west_edge, uint32_t inf_per_sqm)
{
    require_auth(owner);
    auction_table auctions(_self, _self.value);
    auction land_auction = auctions.get(0, "The land auction has not started");
    time_point_sec start_date = land_auction.start_date;
    time_point_sec end_date = start_date + seconds_in_one_week;
    time_point_sec current_date = time_point_sec(now());

    eosio_assert(inf_per_sqm >= reg_inf_per_sqm,
        ("INF per square meter must be at least " + std::to_string(reg_inf_per_sqm)).c_str());

    std::pair<double, double> land_size = assert_lat_long_values(lat_north_edge, long_east_edge,
        lat_south_edge, long_west_edge);

    landbid_table landbids(_self, _self.value);
    auto lat_north_index = landbids.get_index<"bylatnorth"_n>();
    auto landbids_itr = lat_north_index.lower_bound(lat_south_edge);
    // Add max_land_length meters to lat_north_edge to get upper bound
    double upper_bound = lat_north_edge + meters_to_lat_dist(max_land_length);
    bool covers_recent_existing_bid = false;

    while(landbids_itr != lat_north_index.end() && landbids_itr->lat_north_edge < upper_bound)
    {
        landbid bid = *landbids_itr;
        // If an intersecing land is completely inside (covered by) the new bid, it must be outbid
        // Unless the existing bid is by the same user
        if(bid.lat_north_edge <= lat_north_edge &&
            bid.lat_south_edge >= lat_south_edge &&
            bid.long_east_edge <= long_east_edge &&
            bid.long_west_edge >= long_west_edge)
        {
            eosio_assert(inf_per_sqm > bid.inf_per_sqm ||
            (owner == bid.owner && inf_per_sqm == bid.inf_per_sqm),
            "INF per square meter is less than or equal to covered land bids");
            
            cancel_deferred(bid.id);

            std::pair<double, double> covered_bid_land_size = lat_long_to_meters(
                bid.lat_north_edge, bid.long_east_edge, bid.lat_south_edge, bid.long_west_edge);

            asset inf_amount = calculate_land_reg_fee(covered_bid_land_size, bid.inf_per_sqm, 1);
            // Refund INF to covered bid
            transfer_inf(_self, bid.owner, inf_amount, "Your land has been outbid");

            if(current_date < bid.bid_date + seconds_in_one_day)
            {
                covers_recent_existing_bid = true;
            }

            // Erase the covered bid as it has been outbid
            landbids_itr = lat_north_index.erase(landbids_itr);
        }
        // Otherwise, any other kind of intersection is not allowed
        else
        {
            eosio_assert(
                bid.long_east_edge <= long_west_edge ||
                bid.long_west_edge >= long_east_edge ||
                bid.lat_south_edge >= lat_north_edge ||
                // Required because lower bound includes equality case
                bid.lat_north_edge <= lat_south_edge,
                "Bid intersects with existing bids, without completely covering them");
            landbids_itr++;
        }
    }

    // If the auction has ended, it is still possible to make bids that cover existing bids
    // Lands are awarded after they have not been outbid for 24 hours
    if(current_date >= end_date)
    {
        eosio_assert(covers_recent_existing_bid,
        "As the land auction has ended, new bids must cover existing bids made less than 24 hours ago");

        // As the land auction has ended we must also check if there are any intersections in the land table
        land_table lands(_self, _self.value);

        check_land_intersections(lands, lat_north_edge, long_east_edge, lat_south_edge, long_west_edge);
    }

    asset inf_amount = calculate_land_reg_fee(land_size, inf_per_sqm, 1);
    deduct_inf_deposit(owner, inf_amount);

    uint64_t bid_id = landbids.available_primary_key();
    landbids.emplace(owner, [&](auto &row) {
        row.id = bid_id;
        row.owner = owner;
        row.lat_north_edge = lat_north_edge;
        row.long_east_edge = long_east_edge;
        row.lat_south_edge = lat_south_edge;
        row.long_west_edge = long_west_edge;
        row.bid_date = time_point_sec(now());
        row.inf_per_sqm = inf_per_sqm;
    });

    transaction t;
    t.actions.emplace_back(permission_level{owner, "active"_n},
        _self, "awardlandbid"_n,
        std::make_tuple(bid_id)
    );
    
    if(current_date >= end_date)
    {
        t.delay_sec = seconds_in_one_day;
    }
    else
    {
        t.delay_sec = std::max(seconds_in_one_day, end_date.utc_seconds - current_date.utc_seconds);
    }

    t.send(bid_id, owner);
}

/*void infiniverse::cancelbid(uint64_t bid_id)
{
    landbid_table landbids(_self, _self.value);
    landbid bid = landbids.get(bid_id, "Bid Id does not exist");
    require_auth(bid.owner);
    eosio_assert(bid.inf_per_sqm == 1, "You can only cancel 1 INF per square meter bids");

    cancel_deferred(bid.id);

    std::pair<double, double> land_size = lat_long_to_meters(
        bid.lat_north_edge, bid.long_east_edge, bid.lat_south_edge, bid.long_west_edge);

    asset inf_amount = calculate_land_reg_fee(land_size, bid.inf_per_sqm, 1);
    // Refund INF to cancelled bid
    transfer_inf(_self, bid.owner, inf_amount, "You cancelled your bid");

    // Erase the bid as it has been cancelled
    landbids.erase(landbids.find(bid_id));
}*/

void infiniverse::awardlandbid(uint64_t bid_id)
{
    landbid_table landbids(_self, _self.value);
    landbid bid = landbids.get(bid_id, "Bid Id does not exist");
    require_auth(bid.owner);
    
    auction_table auctions(_self, _self.value);
    auction land_auction = auctions.get(0);
    time_point_sec start_date = land_auction.start_date;
    time_point_sec end_date = start_date + seconds_in_one_week;
    time_point_sec current_date = time_point_sec(now());
    time_point_sec bid_award_date = std::max(end_date, bid.bid_date+seconds_in_one_day);

    eosio_assert(current_date >= bid_award_date, "Land bid cannot be awarded yet");

    std::pair<double, double> land_size = lat_long_to_meters(bid.lat_north_edge,
    bid.long_east_edge, bid.lat_south_edge, bid.long_west_edge);

    asset inf_amount = calculate_land_reg_fee(land_size, bid.inf_per_sqm, 1);
    // The bid price gets sent back to the token issuing account
    transfer_inf(_self, inf_account, inf_amount, "Awarded land auction cost");

    land_table lands(_self, _self.value);

    lands.emplace(bid.owner, [&](auto &row) {
        row.id = lands.available_primary_key();
        row.owner = bid.owner;
        row.lat_north_edge = bid.lat_north_edge;
        row.long_east_edge = bid.long_east_edge;
        row.lat_south_edge = bid.lat_south_edge;
        row.long_west_edge = bid.long_west_edge;
        row.reg_end_date = time_point_sec(now() + seconds_in_one_year);
    });
    landbids.erase(landbids.find(bid_id));
}


std::pair<double, double> infiniverse::assert_lat_long_values(const double& lat_north_edge,
        const double& long_east_edge, const double& lat_south_edge, const double& long_west_edge)
{ eosio_assert(lat_north_edge > lat_south_edge, "North edge must have greater latitude than south edge");
    // Temporary restriction of registering land across the antimeridian to simplify land intersection algorithm
    eosio_assert(long_east_edge > long_west_edge, "East edge must have greater longitude than west edge");
    // Temporary longitude limit to between -85 and 85 degrees to simplify display of lands on a mapping UI
    eosio_assert(lat_north_edge < 85, "Latitude cannot be greater than 85 degrees");
    eosio_assert(lat_south_edge > -85, "Latitude cannot be less than -85 degrees");
    eosio_assert(long_east_edge <= 180 && long_east_edge > -180 && long_west_edge <= 180
        && long_west_edge > -180, "Longitude must be between -180 and 180 degrees");

    std::pair<double, double> land_size = lat_long_to_meters(lat_north_edge,
    long_east_edge, lat_south_edge, long_west_edge);

    eosio_assert(land_size.first <= max_land_length && land_size.second <= max_land_length,
        ("Land cannot exceed a length of " + std::to_string(max_land_length) + " meters on either side").c_str());

    return land_size;
}

void infiniverse::check_land_intersections(const land_table& lands, const double& lat_north_edge,
        const double& long_east_edge, const double& lat_south_edge, const double& long_west_edge)
{
    auto lat_north_index = lands.get_index<"bylatnorth"_n>();
    auto lands_itr = lat_north_index.lower_bound(lat_south_edge);
    // Add max_land_length meters to lat_north_edge to get upper bound
    double upper_bound = lat_north_edge + meters_to_lat_dist(max_land_length);

    while(lands_itr != lat_north_index.end() && lands_itr->lat_north_edge < upper_bound)
    {
        eosio_assert(
            lands_itr->long_east_edge <= long_west_edge ||
            lands_itr->long_west_edge >= long_east_edge ||
            lands_itr->lat_south_edge >= lat_north_edge ||
            // Required because lower bound includes equality case
            lands_itr->lat_north_edge <= lat_south_edge,
            "Intersecting land has already been registered");
        lands_itr++;
    }
}

void infiniverse::check_landbid_intersections(const landbid_table& landbids, const double& lat_north_edge,
        const double& long_east_edge, const double& lat_south_edge, const double& long_west_edge)
{
    auto lat_north_index = landbids.get_index<"bylatnorth"_n>();
    auto landbidss_itr = lat_north_index.lower_bound(lat_south_edge);
    // Add max_land_length meters to lat_north_edge to get upper bound
    double upper_bound = lat_north_edge + meters_to_lat_dist(max_land_length);

    while(landbidss_itr != lat_north_index.end() && landbidss_itr->lat_north_edge < upper_bound)
    {
        eosio_assert(
            landbidss_itr->long_east_edge <= long_west_edge ||
            landbidss_itr->long_west_edge >= long_east_edge ||
            landbidss_itr->lat_south_edge >= lat_north_edge ||
            // Required because lower bound includes equality case
            landbidss_itr->lat_north_edge <= lat_south_edge,
            "You cannot register land that intersects with a landbid");
        landbidss_itr++;
    }
}

asset infiniverse::calculate_land_reg_fee(const std::pair<double, double>& land_size, const uint32_t& inf_per_sqm, const double& years)
{
    // Calculate registration fee assuming each side is at least 1 meter to avoid abuse
    // Otherwise a malicious user could register a very thin, long and cheap land
    // This land would be useless but would stop anyone else from registering land over it
    double land_area = std::max(land_size.first, 1.0) * std::max(land_size.second, 1.0);
    return calculate_land_reg_fee(land_area, inf_per_sqm, years);
}

asset infiniverse::calculate_land_reg_fee(const double& land_area, const uint32_t& inf_per_sqm, const double& years)
{
    int64_t reg_price = static_cast<int64_t>(round(land_area * inf_per_sqm * years));
    // multiply fee by 10000 to account for four decimal places of INF
    return asset(reg_price * 10000, inf_symbol);
}

void infiniverse::deduct_inf_deposit(const name& owner, const asset& inf_amount)
{
    deposit_table deposits(_self, _self.value);
    deposit d = deposits.get(owner.value, "User does not have a deposit opened");
    eosio_assert(d.balance >= inf_amount, "User's INF deposit balance is too low");

    deposits.modify(deposits.find(owner.value), same_payer, [&](auto &row){
        row.balance -= inf_amount;
    });
}

uint64_t infiniverse::get_land_id_from_persistent(const persistent_table& persistents, const uint64_t& persistent_id)
{
    persistent p = persistents.get(persistent_id, "Persistent Id does not exist");
    return p.land_id;
}

name infiniverse::require_land_owner_auth(const uint64_t& land_id)
{
    land_table lands(_self, _self.value);
    land l = lands.get(land_id, "Land Id does not exist");
    require_auth(l.owner);
    return l.owner;
}

void infiniverse::assert_vectors_within_bounds(const vector3& position,
    const vector3& orientation, const vector3& scale)
{
    eosio_assert(position.x > 0 && position.y == 0 && position.z > 0 &&
        position.x < 1 && position.z < 1,
        "Asset position is not within land bounds");

    eosio_assert(orientation.x >= 0 && orientation.x < 360 && orientation.y >= 0 &&
        orientation.y < 360 && orientation.z >= 0 && orientation.z < 360,
        "Asset orientation must be within 0 and 360");

    eosio_assert(scale.x >= 0.2 && scale.y >= 0.2 && scale.z >= 0.2, 
        "Asset scale must be at least 0.2");
}

uint64_t infiniverse::add_poly(const name& user, const std::string& poly_id)
{
    require_auth(user);

    eosio_assert((poly_id.length() == 11), "Poly Id format is invalid");

    poly_table poly(_self, _self.value);
    auto user_index = poly.get_index<"byuser"_n>();
    auto poly_itr = user_index.find(user.value);
    while(poly_itr != user_index.end() && poly_itr->user == user)
    {
        if(poly_itr->poly_id == poly_id)
        {
            return poly_itr->id;
        }
        poly_itr++;
    }

    uint64_t new_id = poly.available_primary_key();
    poly.emplace(user, [&](auto &row) {
        row.id = new_id;
        row.user = user;
        row.poly_id = poly_id;
    });
    return new_id;
}

// This function requires giving the active permission to the eosio.code permission
// cleos set account permission infiniverse1 active '{"threshold": 1,"keys": [{"key": "ACTIVE PUBKEY","weight": 1}],"accounts": [{"permission":{"actor":"infiniverse1","permission":"eosio.code"},"weight":1}]}' owner -p infiniverse1@owner
void infiniverse::transfer_inf(name from, name to, asset quantity, std::string memo)
{
    action{
        permission_level{_self, "active"_n},
        inf_account,
        "transfer"_n,
        std::tuple<name, name, asset, std::string>{from, to, quantity, memo}
    }.send();
}

extern "C" {
    void apply(uint64_t receiver, uint64_t code, uint64_t action) {
        if(code==receiver)
        {
            switch(action)
            {
                EOSIO_DISPATCH_HELPER( infiniverse, (registerland)(persistpoly)(updatepersis)(deletepersis)(opendeposit)(closedeposit)(makelandbid)(awardlandbid) )
            }
        }
        else if(code==inf_account.value && action=="transfer"_n.value) {
            execute_action( name(receiver), name(code), &infiniverse::depositinf );
        }
    }
};