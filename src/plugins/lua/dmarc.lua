--[[
Copyright (c) 2011-2015, Vsevolod Stakhov <vsevolod@highsecure.ru>
Copyright (c) 2015, Andrew Lewis <nerf@judo.za.org>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
]]--

-- Dmarc policy filter

local rspamd_regexp = require "rspamd_regexp"
local rspamd_logger = require "rspamd_logger"
local rspamd_redis = require "rspamd_redis"
local upstream_list = require "rspamd_upstream_list"
local rspamd_util = require "rspamd_util"

--local dumper = require 'pl.pretty'.dump

local symbols = {
  spf_allow_symbol = 'R_SPF_ALLOW',
  spf_deny_symbol = 'R_SPF_FAIL',
  spf_softfail_symbol = 'R_SPF_SOFTFAIL',
  spf_neutral_symbol = 'R_SPF_NEUTRAL',

  dkim_allow_symbol = 'R_DKIM_ALLOW',
  dkim_deny_symbol = 'R_DKIM_REJECT',
}
-- Default port for redis upstreams
local default_port = 6379
local upstreams = nil
local dmarc_redis_key_prefix = "dmarc_"
local dmarc_domain = nil
local elts_re = rspamd_regexp.create_cached("\\\\{0,1};\\s+")

local function dmarc_report(task, spf_ok, dkim_ok)
  local ip = task:get_from_ip()
  if not ip:is_valid() then
    return nil
  end
  local res = string.format('%d,%s,%s,%s', task:get_date(0),
    ip:to_string(), tostring(spf_ok), tostring(dkim_ok))

  return res
end

local function dmarc_callback(task)
  local from = task:get_from(2)
  local dmarc_domain

  if from and from[1] and from[1]['domain'] and not from[2] then
    dmarc_domain = rspamd_util.get_tld(from[1]['domain'])
  else
    return
  end

  local function dmarc_report_cb(task, err, data)
    if not err then
      rspamd_logger.infox(task, '<%1> dmarc report saved for %2',
        task:get_message_id(), from[1]['domain'])
    else
      rspamd_logger.errx(task, '<%1> dmarc report is not saved for %2: %3',
        task:get_message_id(), from[1]['domain'], err)
    end
  end

  local function dmarc_dns_cb(resolver, to_resolve, results, err, key)

    local lookup_domain = string.sub(to_resolve, 8)
    if not results then
      if lookup_domain ~= dmarc_domain then
        local resolve_name = '_dmarc.' .. dmarc_domain
        task:get_resolver():resolve_txt({
          task=task,
          name = resolve_name,
          callback = dmarc_dns_cb})
        return
      end

      return
    end

    local strict_spf = false
    local strict_dkim = false
    local strict_policy = false
    local quarantine_policy = false
    local found_policy = false
    local failed_policy = false
    local rua

    for _,r in ipairs(results) do
      if failed_policy then break end
      (function()
        if not string.match(r, '^v=DMARC1[;\\][; ]') then
          return
        else
          if found_policy then
            failed_policy = true
            return
          else
            found_policy = true
          end
        end
        local elts = elts_re:split(r)

        if elts then
          for _,e in ipairs(elts) do
            dkim_pol = string.match(e, '^adkim=(.)$')
            if dkim_pol then
              if dkim_pol == 's' then
                strict_dkim = true
              elseif dkim_pol ~= 'r' then
                failed_policy = true
                return
              end
            end
            spf_pol = string.match(e, '^aspf=(.)$')
            if spf_pol then
              if spf_pol == 's' then
                strict_spf = true
              elseif spf_pol ~= 'r' then
                failed_policy = true
                return
              end
            end
            policy = string.match(e, '^p=(.+)$')
            if policy then
              if (policy == 'reject') then
                strict_policy = true
              elseif (policy == 'quarantine') then
                strict_policy = true
                quarantine_policy = true
              elseif (policy ~= 'none') then
                failed_policy = true
                return
              end
            end
            subdomain_policy = string.match(e, '^sp=(.+)$')
            if subdomain_policy and lookup_domain == dmarc_domain then
              if (subdomain_policy == 'reject') then
                if dmarc_domain ~= from[1]['domain'] then
                  strict_policy = true
                end
              elseif (subdomain_policy == 'quarantine') then
                if dmarc_domain ~= from[1]['domain'] then
                  strict_policy = true
                  quarantine_policy = true
                end
              elseif (subdomain_policy == 'none') then
                if dmarc_domain ~= from[1]['domain'] then
                  strict_policy = false
                  quarantine_policy = false
                end
              else
                failed_policy = true
                return
              end
            end
            pct = string.match(e, '^pct=(%d+)$')
            if pct then
              pct = tonumber(pct)
            end

            if not rua then
              rua = string.match(e, '^rua=([^%s]+)$')
            end
          end
        end
      end)()
    end

    if not found_policy then
      if lookup_domain ~= dmarc_domain then
        local resolve_name = '_dmarc.' .. dmarc_domain
        task:get_resolver():resolve_txt({
          task=task,
          name = resolve_name,
          callback = dmarc_dns_cb})

        return
      else
        return
      end
    end

    if failed_policy then return end

    -- Check dkim and spf symbols
    local spf_ok = false
    local dkim_ok = false
    if task:has_symbol(symbols['spf_allow_symbol']) then
      efrom = task:get_from(1)
      if efrom and efrom[1] and efrom[1]['domain'] then
        if rspamd_util.strequal_caseless(efrom[1]['domain'], from[1]['domain']) then
          spf_ok = true
        elseif not strict_spf then
          if rspamd_util.strequal_caseless(
              string.sub(efrom[1]['domain'], -string.len('.' .. lookup_domain)),
              '.' .. lookup_domain) then
            spf_ok = true
          end
        end
      end
    end
    local das = task:get_symbol(symbols['dkim_allow_symbol'])
    if das and das[1] and das[1]['options'] then
      for i,dkim_domain in ipairs(das[1]['options']) do
        if rspamd_util.strequal_caseless(from[1]['domain'], dkim_domain) then
          dkim_ok = true
        elseif not strict_dkim then
          if rspamd_util.strequal_caseless(
              string.sub(dkim_domain, -string.len('.' .. lookup_domain)),
              '.' .. lookup_domain) then
            dkim_ok = true
          end
        end
      end
    end

    local res = 0.5
    if not (spf_ok or dkim_ok) then
      res = 1.0
      if quarantine_policy then
        if not pct or pct == 100 or (math.random(100) <= pct) then
          task:insert_result('DMARC_POLICY_QUARANTINE', res, lookup_domain)
        end
      elseif strict_policy then
        if not pct or pct == 100 or (math.random(100) <= pct) then
          task:insert_result('DMARC_POLICY_REJECT', res, lookup_domain)
        end
      else
        task:insert_result('DMARC_POLICY_SOFTFAIL', res, lookup_domain)
      end
    else
      task:insert_result('DMARC_POLICY_ALLOW', res, lookup_domain)
    end

    if rua and not(spf_ok or dkim_ok) and upstreams then
      -- Prepare and send redis report element
      local upstream = upstreams:get_upstream_by_hash(from[1]['domain'])
      local redis_key = dmarc_redis_key_prefix .. from[1]['domain']
      local addr = upstream:get_addr()
      local report_data = dmarc_report(task, spf_ok, dkim_ok)

      if report_data then
        rspamd_redis.make_request(task, addr, dmarc_report_cb,
          'LPUSH', {redis_key, report_data})
      end
    end

    -- XXX: handle rua and push data to redis
  end

  -- Do initial request
  local resolve_name = '_dmarc.' .. from[1]['domain']
  task:get_resolver():resolve_txt({
    task=task,
    name = resolve_name,
    callback = dmarc_dns_cb})
end

local opts = rspamd_config:get_all_opt('dmarc')
if not opts or type(opts) ~= 'table' then
  return
end

if not opts['servers'] then
  rspamd_logger.infox(rspamd_config, 'no servers are specified for dmarc stats')
else
  upstreams = upstream_list.create(rspamd_config, opts['servers'], default_port)
  if not upstreams then
    rspamd_logger.errx(rspamd_config, 'cannot parse servers parameter')
  end
end

if opts['key_prefix'] then
  dmarc_redis_key_prefix = opts['key_prefix']
end

-- Check spf and dkim sections for changed symbols
local function check_mopt(var, opts, name)
  if opts[name] then
    symbols['var'] = tostring(opts[name])
  end
end

local spf_opts = rspamd_config:get_all_opt('spf')
if spf_opts then
  check_mopt('spf_deny_symbol', spf_opts, 'symbol_fail')
  check_mopt('spf_allow_symbol', spf_opts, 'symbol_allow')
  check_mopt('spf_softfail_symbol', spf_opts, 'symbol_softfail')
  check_mopt('spf_neutral_symbol', spf_opts, 'symbol_neutral')
end

local dkim_opts = rspamd_config:get_all_opt('dkim')
if dkim_opts then
  check_mopt('dkim_deny_symbol', 'symbol_reject')
  check_mopt('dkim_allow_symbol', 'symbol_allow')
end

local id = rspamd_config:register_callback_symbol('DMARC_CALLBACK', 1.0,
  dmarc_callback)
rspamd_config:register_virtual_symbol('DMARC_POLICY_ALLOW', -1, id)
rspamd_config:register_virtual_symbol('DMARC_POLICY_REJECT', 1, id)
rspamd_config:register_virtual_symbol('DMARC_POLICY_QUARANTINE', 1, id)
rspamd_config:register_virtual_symbol('DMARC_POLICY_SOFTFAIL', 1, id)
rspamd_config:register_dependency(id, symbols['spf_allow_symbol'])
rspamd_config:register_dependency(id, symbols['dkim_allow_symbol'])

