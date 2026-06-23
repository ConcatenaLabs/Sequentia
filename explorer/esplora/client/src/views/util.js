import moveDec from 'move-decimal-point'
import { sat2btc } from 'fmtbtc'
import { nativeAssetId, nativeAssetLabel } from '../const'
import { isNativeOut } from '../util'

const DEFAULT_ISSUED_PRECISION = 0
    , NATIVE_PRECISION = 8

const pad = n => n < 10 ? '0'+n : n

const formatTimezone = time => {
  const tzOffset = time.getTimezoneOffset() * -1;
  return tzOffset == 0 ? 'UTC' : 'GMT ' + (tzOffset < 0 ? '' : '+') + (tzOffset/60)
}

export const formatTime = (unix, with_tz = true) => {
  const time = new Date(unix*1000)

  return `${time.getFullYear()}-${pad(time.getMonth() + 1)}-${pad(time.getDate())}`
       + ` ${pad(time.getHours())}:${pad(time.getMinutes())}:${pad(time.getSeconds())}`
       + (with_tz ? ' ' + formatTimezone(time) : '')
}

export const formatSat = (sats, label=nativeAssetLabel) => `${formatNumber(sat2btc(sats), NATIVE_PRECISION)} ${label}`

export const formatAssetAmount = (value, precision=0, t) =>
  <span>
    {formatNumber(precision > 0 ? moveDec(value, -precision) : value, precision)}
  </span>

export const formatOutAmount = (vout, { t, assetMap }, shortDisplay=false) => {
  if (vout.value == null) return t`Confidential`

  if (isNativeOut(vout)) {
    return <span>
      {formatNumber(sat2btc(vout.value), NATIVE_PRECISION)}
      { ' ' }
      {!vout.asset ? nativeAssetLabel : <a href={`asset/${vout.asset}`}>{nativeAssetLabel}</a>}
    </span>
  }

  const [ domain, ticker, name, _precision ] = vout.asset && assetMap && assetMap[vout.asset] || []
      , precision = _precision != null ? _precision : DEFAULT_ISSUED_PRECISION
      , short_id = vout.asset && vout.asset.substr(0, 10)
      , asset_url = vout.asset && `asset/${vout.asset}`

  const amount_el = formatAssetAmount(vout.value, precision, t)
      , asset_link = vout.asset && <a href={asset_url}>{short_id}</a>

  return domain ? <span>{amount_el} {ticker && <span title={name}>{ticker}</span>} {shortDisplay||<br />} {domain}{shortDisplay || [<br/>,<em title={vout.asset}>{asset_link}</em>]}</span>
       : vout.asset ? <span>{amount_el} <em title={vout.asset}>{asset_link}</em></span>
       : <span>{amount_el} {t`Unknown`}</span> // should never happen
}

// SEQUENTIA: format a raw (asset, value) pair as a compact "amount TICKER" string,
// using the asset registry for ticker + precision. The native asset and unknown
// assets are both handled; an unknown asset falls back to a short id. Returns a
// plain string (not JSX) so it can be used in tables, hover titles and links.
export const formatAssetValue = (asset, value, assetMap = {}) => {
  if (value == null) return ''
  if (!asset || asset === nativeAssetId) {
    return `${formatNumber(sat2btc(value), NATIVE_PRECISION)} ${nativeAssetLabel}`
  }
  const [ , ticker, , _precision ] = (assetMap && assetMap[asset]) || []
      , precision = _precision != null ? _precision : DEFAULT_ISSUED_PRECISION
      , amount = formatNumber(precision > 0 ? moveDec(value, -precision) : value, precision)
      , label = ticker || `${asset.substr(0, 6)}…`
  return `${amount} ${label}`
}

// SEQUENTIA: the ticker for an asset (native asset and unknown assets handled).
export const tickerOf = (asset, assetMap = {}) => {
  if (!asset || asset === nativeAssetId) return nativeAssetLabel
  const meta = assetMap && assetMap[asset]
  return (meta && meta[1]) || `${asset.substr(0, 6)}…`
}

// SEQUENTIA: a fee rate per vByte (in the fee asset's base units). One decimal for
// normal rates; for sub-unit rates show enough precision to avoid a misleading 0.0.
export const formatFeeRate = rate =>
  !isFinite(rate) || rate <= 0 ? '0'
  : rate >= 1 ? rate.toFixed(1)
  : parseFloat(rate.toPrecision(2)).toString()

// SEQUENTIA: list every explicit (asset, value) transfer equally — no headline/primary asset.
// In the open fee market the native asset (tSEQ) is often just change or fee, so promoting any
// single asset misleads; show them all compactly, e.g. "1,000 USDX, 0.5 tSEQ".
export const formatAssetValues = (outValues, assetMap = {}) =>
  outValues.map(v => formatAssetValue(v.asset, v.value, assetMap)).join(', ')

// SEQUENTIA: per-asset totals of a full tx's EXPLICIT outputs (for the detail view), excluding
// the fee output and any blinded output. Mirrors the backend's `out_values` (overview), so a tx
// with a blinded CHANGE output but an explicit payment still shows the payment, not "Confidential".
export const explicitOutValues = vout => {
  const totals = new Map()
  for (const o of vout || []) {
    if (o.scriptpubkey_type === 'fee') continue
    if (o.value == null) continue // blinded value
    const asset = o.asset || nativeAssetId
    totals.set(asset, (totals.get(asset) || 0) + o.value)
  }
  return [...totals].map(([asset, value]) => ({ asset, value }))
}

export const formatHex = num => {
  const str = num.toString(16)
  return '0x' + (str.length%2 ? '0' : '') + str
}

// Formats a number for display. Treats the number as a string to avoid rounding errors.
export const formatNumber = (s, precision=null) => {
  let [ whole, dec ] = s.toString().split('.')

  // divide numbers into groups of three separated with a thin space (U+202F, "NARROW NO-BREAK SPACE"),
  // but only when there are more than a total of 5 non-decimal digits.
  // if (whole.length >= 5) {
  //   whole = whole.replace(/\B(?=(\d{3})+(?!\d))/g, "\u202F")
  // }

  if (precision != null && precision > 0) {
    if (dec == null) dec = '0'.repeat(precision)
    else if (dec.length < precision) dec += '0'.repeat(precision - dec.length)
  }

  return whole + (dec != null ? '.'+dec : '')
}

export const formatRelativeTime = (fromDate, toDate = new Date()) => {
  if (typeof fromDate === 'number') {
    fromDate = fromDate < 1e12
      ? new Date(fromDate * 1000)
      : new Date(fromDate)
  }

  const diffSeconds = Math.floor((toDate - fromDate) / 1000)

  if (diffSeconds < 5) return 'just now'
  if (diffSeconds < 60) return `${diffSeconds} seconds ago`

  const diffMinutes = Math.floor(diffSeconds / 60)
  if (diffMinutes < 60) {
    return diffMinutes === 1 ? '1 minute ago' : `${diffMinutes} minutes ago`
  }

  const diffHours = Math.floor(diffMinutes / 60)
  if (diffHours < 24) {
    return diffHours === 1 ? '1 hour ago' : `${diffHours} hours ago`
  }

  const diffDays = Math.floor(diffHours / 24)
  return diffDays === 1 ? '1 day ago' : `${diffDays} days ago`
}

export const getBlockPercentageUsed = blockWeight =>
  Math.round((blockWeight / 4000000) * 10000) / 100

export const formatJson = obj =>
  JSON.stringify(obj, null, 1)
    //.replace(/^ /mg, '')
    //.replace(/^\{|\}$/g, '')

// Rewrite a loopback URL's host to the host the page was actually loaded from
// (keeping the port), so links to sibling explorer instances (e.g. our Bitcoin
// testnet4 explorer on :5002) work over localhost, Tailscale, or a reverse proxy.
// Non-loopback (external) URLs are left untouched, and SSR returns the URL as-is.
export const rebaseHost = url => {
  if (!process.browser) return url
  try {
    const u = new URL(url, window.location.href)
    if (['127.0.0.1', 'localhost', '::1', '[::1]'].includes(u.hostname)) {
      u.protocol = window.location.protocol
      u.hostname = window.location.hostname
    }
    return u.toString()
  } catch (e) {
    return url
  }
}

const parentChainExplorerTxOut = process.env.PARENT_CHAIN_EXPLORER_TXOUT || '/tx/{txid}?output:{vout}'
const parentChainExplorerAddr  = process.env.PARENT_CHAIN_EXPLORER_ADDR || '/address/{addr}'
const parentChainExplorerBlock = process.env.PARENT_CHAIN_EXPLORER_BLOCK || 'https://mempool.space/testnet4/block/{hash}'

export const linkToParentOut = ({ txid, vout }, label=`${txid}:${vout}`) =>
  <a href={parentChainExplorerTxOut.replace('{txid}', txid).replace('{vout}', vout)} target="_blank" rel="external">{label}</a>

export const linkToParentAddr = (addr, label=addr) =>
  <a href={parentChainExplorerAddr.replace('{addr}', addr)} target="_blank" rel="external">{label}</a>

// SEQUENTIA: link a block's Bitcoin anchor to the parent chain's explorer (our
// own Bitcoin testnet4 esplora instance), host-relative via rebaseHost.
export const linkToParentBlock = (hash, label=hash) =>
  <a href={rebaseHost(parentChainExplorerBlock.replace('{hash}', hash))} target="_blank" rel="noopener noreferrer">{label}</a>

export const linkToAddr = addr => <a href={`address/${addr}`}>{addr}</a>

export const formatVMB = (bytes, suffix) =>
  bytes >= 10000 || bytes == 0 ? `${(bytes / 1000000).toFixed(2)} ${suffix ?? "vMB"}`
: `< 0.01 ${suffix ?? "vMB"}`


export const strTruncate  = (str) => str.substr(0, 10) + '...' + str.substr(str.length-4, str.length);

export const truncateTxid  = (txid) => txid.substr(0, 5) + '...' + txid.substr(txid.length - 5, txid.length);

// Convert hex string to base64
export const hexToBase64 = (hex) => {
    return Buffer.from(hex, 'hex').toString('base64')
}
