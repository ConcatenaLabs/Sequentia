import { formatSat, formatNumber, truncateTxid, formatAssetValue, formatAssetValues, tickerOf, formatFeeRate } from "./util";
import { nativeAssetId } from "../const";
import loader from "../components/loading";
import { CopyIcon, TxArrowsIcon } from "../components/icons";

const staticRoot = process.env.STATIC_ROOT || "";

const feeRateClass = (feerate, feeEst) => {
  if (!feeEst || feeEst[3] == null || feeEst[12] == null) return "";

  return feerate <= feeEst[12]
    ? "success"
    : feerate <= feeEst[3]
      ? "warning"
      : "danger";
}

// SEQUENTIA: the VALUE column. The Elements/Sequentia backend exposes per-asset
// explicit output totals (`out_values`) and a `confidential` flag. Show EVERY explicit
// transfer equally (no single "primary" — the native asset is often just change/fee in the
// open fee market). Only print "Confidential" when there are NO explicit outputs at all: a
// blinded CHANGE output alongside an explicit payment must not hide the known payment.
// The Bitcoin build (no `out_values`) keeps the original single-value behaviour.
const renderTxValue = (txo, assetMap) => {
  if (txo.out_values === undefined) {
    return txo.value != null ? formatSat(txo.value) : "Confidential";
  }
  if (!txo.out_values.length) return txo.confidential ? "Confidential" : "—";
  return formatAssetValues(txo.out_values, assetMap);
}

// SEQUENTIA: the FEE column. Fees may be paid in any asset (open fee market). Keep
// the useful per-vByte rate, but denominate it in the asset the fee was actually
// paid in (e.g. "50.0 tSEQ/vB", "0.015 USDX/vB") instead of a native-only "sat/vB".
const feeIsNative = txo => txo.fee_asset === undefined || !txo.fee_asset || txo.fee_asset === nativeAssetId;

const renderTxFee = (txo, assetMap) => {
  const rate = txo.fee / txo.vsize;
  if (txo.fee_asset === undefined) return `${formatFeeRate(rate)} sat/vB`; // Bitcoin build
  // Tooltip carries the absolute fee in the same asset for context.
  return <span title={formatAssetValue(txo.fee_asset, txo.fee, assetMap)}>
    {`${formatFeeRate(rate)} ${tickerOf(txo.fee_asset, assetMap)}/vB`}
  </span>;
}

export const transactions = (txs, viewMore, { t, ...S }) => (
  <div className="tx-container">
    {!txs ? (
      loader()
    ) : !txs.length ? (
      <p>{t`No recent transactions`}</p>
    ) : (
      <div className="transaction-table">
        <div className="table-header">
          <div className="table-header-icon-container">
            <TxArrowsIcon />
          </div>
          <h1 className="table-header-title">Latest Transactions</h1>
        </div>

        <div className="table-title-row">
          <div className="transaction-table-transaction-id">TRANSACTION ID</div>
          <div className="transaction-table-transaction-value">VALUE</div>
          <div className="transaction-table-transaction-size">SIZE</div>
          <div className="transaction-table-transaction-fee">
            FEE
          </div>
        </div>

        <div className="transaction-table-body">
          {txs.map((txOverview) => {
            const feerate = txOverview.fee / txOverview.vsize;
            // Colour-code the rate only when the fee market applies (native asset).
            const feeClass = feeIsNative(txOverview) ? feeRateClass(feerate, S.feeEst) : "";
            return (
              <a href={`tx/${txOverview.txid}`}>
              <div className={`transaction-table-row ${S.newTxEntries && S.newTxEntries[txOverview.txid] ? "new-table-entry" : ""}`}>
                <div className="transaction-table-transaction-id">
                  <p>{truncateTxid(txOverview.txid)}</p>
                  <div
                    className="table-copy-button code-button-btn"
                    role="button"
                    tabindex="0"
                    data-clipboardCopy={txOverview.txid}
                    aria-label={`Copy transaction id ${txOverview.txid}`}
                  >
                    <CopyIcon />
                  </div>
                </div>
                <div className="transaction-table-transaction-value">
                  {renderTxValue(txOverview, S.assetMap)}
                </div>
                <div className="transaction-table-transaction-size">{`${formatNumber(txOverview.vsize)} vB`}</div>
                <div className={`transaction-table-transaction-fee ${feeClass}`}>{renderTxFee(txOverview, S.assetMap)}</div>
              </div>
              </a>
            );
          })}
        </div>

        {txs && viewMore ? (
          <a className="view-more font-link-semibold" href="tx/recent">
            <span>{t`See more`}</span>
            <div>
              <img alt="" src={`${staticRoot}img/icons/arrow-right-blue.svg`} />
            </div>
          </a>
        ) : (
          ""
        )}
      </div>
    )}
  </div>
);
